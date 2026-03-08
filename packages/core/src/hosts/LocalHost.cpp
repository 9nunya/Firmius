#include "hosts/LocalHost.hpp"
#include "utils/StringUtil.hpp"
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @file LocalHost.cpp
 * @brief Implementation of the local machine execution host.
 */

namespace {
/**
 * @brief Sets a file descriptor to non-blocking mode.
 */
void setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
} // namespace

LocalHostProcess::LocalHostProcess(pid_t pid, int stdoutFd, int stderrFd,
                                   int stdinFd)
    : pid(pid), stdoutFd(stdoutFd), stderrFd(stderrFd), stdinFd(stdinFd) {
  startTime = std::chrono::steady_clock::now();
  setNonBlocking(stdoutFd);
  setNonBlocking(stderrFd);
  captureThread = std::thread(&LocalHostProcess::captureLoop, this);
}

LocalHostProcess::~LocalHostProcess() {
  if (captureThread.joinable()) {
    captureThread.join();
  }
  if (stdoutFd != -1)
    close(stdoutFd);
  if (stderrFd != -1)
    close(stderrFd);
  if (stdinFd != -1)
    close(stdinFd);
}

void LocalHostProcess::onOutput(
    std::function<void(const std::string &, bool isError)> cb) {
  std::lock_guard<std::mutex> lock(callbackMutex);
  callback = cb;
}

shared::ProcessResult LocalHostProcess::wait() {
  if (captureThread.joinable()) {
    captureThread.join();
  }

  int status;
  waitpid(pid, &status, 0);
  exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  finished = true;

  auto end = std::chrono::steady_clock::now();
  double duration =
      std::chrono::duration<double, std::milli>(end - startTime).count();

  shared::ProcessResult res;
  res.exitCode = exitCode;
  res.stdoutData = stdoutBuffer;
  res.stderrData = stderrBuffer;
  res.durationMs = duration;
  res.finishReason = shared::ProcessFinishReason::Natural;
  return res;
}

shared::ProcessSnapshot LocalHostProcess::inspect() const {
  std::lock_guard<std::mutex> lock(callbackMutex);
  auto now = std::chrono::steady_clock::now();
  double elapsed =
      std::chrono::duration<double, std::milli>(now - startTime).count();

  return {!finished.load(), exitCode, stdoutBuffer, stderrBuffer, elapsed};
}

void LocalHostProcess::kill() { ::kill(pid, SIGKILL); }

void LocalHostProcess::write(const std::string &data) {
  if (stdinFd == -1 || finished)
    return;
  size_t totalWritten = 0;
  while (totalWritten < data.size()) {
    ssize_t res = ::write(stdinFd, data.data() + totalWritten,
                          data.size() - totalWritten);
    if (res < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      throw std::runtime_error("Write to process failed");
    }
    if (res == 0)
      break;
    totalWritten += res;
  }
}

bool LocalHostProcess::isRunning() {
  if (finished)
    return false;
  int status;
  pid_t res = waitpid(pid, &status, WNOHANG);
  if (res == 0)
    return true;
  if (res == pid) {
    exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    finished = true;
    return false;
  }
  return false;
}

std::string LocalHostProcess::getSystemId() const {
  return std::to_string(pid);
}

void LocalHostProcess::captureLoop() {
  struct pollfd fds[2];
  fds[0].fd = stdoutFd;
  fds[0].events = POLLIN;
  fds[1].fd = stderrFd;
  fds[1].events = POLLIN;

  char buf[4096];
  while (fds[0].fd != -1 || fds[1].fd != -1) {
    int res = poll(fds, 2, 100);
    if (res < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd == -1)
        continue;

      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        ssize_t bytes = read(fds[i].fd, buf, sizeof(buf));
        if (bytes > 0) {
          std::string data(buf, bytes);
          std::function<void(const std::string &, bool)> currentCallback;
          {
            std::lock_guard<std::mutex> lock(callbackMutex);
            if (i == 0)
              stdoutBuffer += data;
            else
              stderrBuffer += data;
            currentCallback = callback;
          }
          if (currentCallback)
            currentCallback(data, i == 1);
        } else if (bytes == 0 ||
                   (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
          close(fds[i].fd);
          if (i == 0)
            stdoutFd = -1;
          else
            stderrFd = -1;
          fds[i].fd = -1;
        }
      }
    }
  }
  finished = true;
}

std::string LocalHost::init() { return "localhost"; }
void LocalHost::destroy() {}

void LocalHost::cleanup() {
  std::lock_guard<std::mutex> lock(bgMutex);
  for (auto &[id, proc] : backgroundProcesses) {
    if (proc)
      proc->kill();
  }
  backgroundProcesses.clear();
}

void LocalHost::setUser(const std::string &user) { currentUser = user; }

std::vector<uint8_t> LocalHost::readFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open())
    throw std::runtime_error("Could not open file: " + path);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
}

void LocalHost::writeFile(const std::string &path,
                          const std::vector<uint8_t> &data) {
  auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  std::ofstream file(path, std::ios::binary);
  if (!file.is_open())
    throw std::runtime_error("Could not open file for writing: " + path);
  file.write(reinterpret_cast<const char *>(data.data()), data.size());
}

bool LocalHost::exists(const std::string &path) {
  return std::filesystem::exists(path);
}

std::vector<shared::FileInfo> LocalHost::listDir(const std::string &path) {
  std::filesystem::path dirPath(path);
  if (!std::filesystem::is_directory(dirPath)) {
    throw std::runtime_error("Not a directory: " + path);
  }
  std::vector<shared::FileInfo> entries;
  for (const auto &entry : std::filesystem::directory_iterator(dirPath)) {
    std::error_code ec;
    auto status = entry.symlink_status(ec);
    if (ec)
      continue;
    bool isSymlink = std::filesystem::is_symlink(status);
    bool isDir = entry.is_directory(ec);
    uint64_t size = 0;
    if (!isDir && !ec) {
      size = entry.file_size(ec);
      if (ec)
        size = 0;
    }
    auto ftime = entry.last_write_time(ec);
    int64_t modMs = 0;
    if (!ec) {
      auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
          std::chrono::file_clock::to_sys(ftime));
      modMs = sctp.time_since_epoch().count();
    }
    entries.push_back({entry.path().filename().string(), entry.path().string(),
                       size, isDir, isSymlink, modMs});
  }
  return entries;
}

shared::FileInfo LocalHost::stat(const std::string &path) {
  std::filesystem::path fspath(path);
  if (!std::filesystem::exists(fspath)) {
    throw std::runtime_error("Path does not exist: " + path);
  }
  std::error_code ec;
  auto status = std::filesystem::symlink_status(fspath, ec);
  if (ec)
    throw std::runtime_error("Failed to stat: " + path + " (" + ec.message() +
                             ")");
  bool isSymlink = std::filesystem::is_symlink(status);
  auto resolvedStatus = std::filesystem::status(fspath, ec);
  bool isDir = std::filesystem::is_directory(resolvedStatus);
  uint64_t size = 0;
  if (!isDir) {
    size = std::filesystem::file_size(fspath, ec);
    if (ec)
      size = 0;
  }
  auto ftime = std::filesystem::last_write_time(fspath, ec);
  int64_t modMs = 0;
  if (!ec) {
    auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::file_clock::to_sys(ftime));
    modMs = sctp.time_since_epoch().count();
  }
  return {fspath.filename().string(),
          fspath.string(),
          size,
          isDir,
          isSymlink,
          modMs};
}

shared::ProcessResult
LocalHost::exec(const std::string &command, const std::string &cwd,
                const std::map<std::string, std::string> &env,
                std::optional<std::chrono::milliseconds> timeout) {
  auto start = std::chrono::steady_clock::now();
  auto proc = spawn(command, cwd, env);

  // We don't have an ID here, but we can still capture output if needed.
  // However, exec is usually for synchronous small tasks.
  // If we want EngineEvents, we need an ID.

  if (!timeout.has_value()) {
    auto res = proc->wait();
    res.finishReason = ProcessFinishReason::Natural;
    res.durationMs = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    return res;
  }

  auto deadline = start + *timeout;

  while (true) {
    if (!proc->isRunning()) {
      auto res = proc->wait();
      res.finishReason = ProcessFinishReason::Natural;
      res.durationMs = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - start)
                           .count();
      return res;
    }

    auto snapshot = proc->inspect();
    auto now = std::chrono::steady_clock::now();

    if (now >= deadline) {
      auto elapsed =
          std::chrono::duration<double, std::milli>(now - start).count();
      std::string bgId = StringUtil::generateUuid();
      registerBackgroundProcess(bgId, std::move(proc));

      shared::ProcessResult partial;
      partial.exitCode = -1;
      partial.stdoutData = snapshot.stdoutData;
      partial.stderrData = snapshot.stderrData;
      partial.durationMs = elapsed;
      partial.finishReason = ProcessFinishReason::Timeout;
      partial.backgroundProcessId = bgId;

      return partial;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

std::unique_ptr<shared::IHostProcess>
LocalHost::spawn(const std::string &command, const std::string &cwd,
                 const std::map<std::string, std::string> &env) {
  int outPipe[2], errPipe[2], inPipe[2];
  if (pipe(outPipe) != 0 || pipe(errPipe) != 0 || pipe(inPipe) != 0)
    throw std::runtime_error("Pipe failed");

  pid_t pid = fork();
  if (pid == 0) {
    if (!currentUser.empty()) {
      struct passwd *pw = getpwnam(currentUser.c_str());
      if (pw)
        setuid(pw->pw_uid);
    }

    if (!cwd.empty()) {
      std::error_code ec;
      std::filesystem::current_path(cwd, ec);
      if (ec)
        _exit(127);
    }

    for (const auto &[k, v] : env)
      setenv(k.c_str(), v.c_str(), 1);

    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    dup2(errPipe[1], STDERR_FILENO);
    close(inPipe[0]);
    close(inPipe[1]);
    close(outPipe[0]);
    close(outPipe[1]);
    close(errPipe[0]);
    close(errPipe[1]);

    execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);
  } else if (pid > 0) {
    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);
    return std::make_unique<LocalHostProcess>(pid, outPipe[0], errPipe[0],
                                              inPipe[1]);
  } else {
    throw std::runtime_error("Fork failed");
  }
}

void LocalHost::registerBackgroundProcess(const std::string &id,
                                          std::unique_ptr<IHostProcess> proc) {
  std::lock_guard<std::mutex> lock(bgMutex);
  backgroundProcesses[id] = std::move(proc);
}

shared::ProcessSnapshot
LocalHost::inspectBackgroundProcess(const std::string &id) {
  std::lock_guard<std::mutex> lock(bgMutex);
  auto it = backgroundProcesses.find(id);
  if (it == backgroundProcesses.end()) {
    throw std::runtime_error("Background process not found: " + id);
  }
  auto snapshot = it->second->inspect();
  if (!snapshot.running) {
    backgroundProcesses.erase(it);
  }
  return snapshot;
}

void LocalHost::writeToBackgroundProcess(const std::string &id,
                                         const std::string &data) {
  std::lock_guard<std::mutex> lock(bgMutex);
  auto it = backgroundProcesses.find(id);
  if (it == backgroundProcesses.end()) {
    throw std::runtime_error("Background process not found: " + id);
  }
  it->second->write(data);
}

void LocalHost::killBackgroundProcess(const std::string &id) {
  std::lock_guard<std::mutex> lock(bgMutex);
  auto it = backgroundProcesses.find(id);
  if (it != backgroundProcesses.end()) {
    it->second->kill();
  } else {
    throw std::runtime_error("Background process not found: " + id);
  }
}

} // namespace firmius::core
