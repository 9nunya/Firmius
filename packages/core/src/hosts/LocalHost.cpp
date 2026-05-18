#include "hosts/LocalHost.hpp"
#include "utils/StringUtil.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace firmius::core {

using namespace firmius::shared;

namespace {
#if defined(_WIN32)
std::string escapeForCmd(const std::string &command) {
  std::string escaped;
  escaped.reserve(command.size() + 8);
  for (char ch : command) {
    if (ch == '"') {
      escaped += '\\';
    }
    escaped += ch;
  }
  return escaped;
}

std::string buildWindowsShellCommand(const std::string &command) {
  return "cmd.exe /S /C \"" + escapeForCmd(command) + "\"";
}

std::vector<char>
buildWindowsEnvironmentBlock(const std::map<std::string, std::string> &overrides) {
  std::map<std::string, std::string> merged;

  LPCH env = GetEnvironmentStringsA();
  if (env) {
    for (LPCH it = env; *it != '\0'; it += std::strlen(it) + 1) {
      std::string entry(it);
      auto sep = entry.find('=');
      if (sep == std::string::npos || sep == 0) {
        continue;
      }
      merged[entry.substr(0, sep)] = entry.substr(sep + 1);
    }
    FreeEnvironmentStringsA(env);
  }

  for (const auto &[k, v] : overrides) {
    merged[k] = v;
  }

  std::vector<char> block;
  for (const auto &[k, v] : merged) {
    std::string line = k + "=" + v;
    block.insert(block.end(), line.begin(), line.end());
    block.push_back('\0');
  }
  block.push_back('\0');
  return block;
}
#else
void setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}
#endif
} // namespace

struct LocalHostProcess::Impl {
#if defined(_WIN32)
  HANDLE processHandle = nullptr;
  HANDLE threadHandle = nullptr;
  HANDLE stdoutRead = nullptr;
  HANDLE stderrRead = nullptr;
  HANDLE stdinWrite = nullptr;
  DWORD pid = 0;
#else
  pid_t pid = -1;
  pid_t processGroupId = -1;
  int stdoutFd = -1;
  int stderrFd = -1;
  int stdinFd = -1;
#endif
};

LocalHostProcess::LocalHostProcess(std::unique_ptr<Impl> impl)
    : impl(std::move(impl)) {
  startTime = std::chrono::steady_clock::now();
#if !defined(_WIN32)
  setNonBlocking(this->impl->stdoutFd);
  setNonBlocking(this->impl->stderrFd);
#endif
  stdoutThread = std::thread(&LocalHostProcess::captureLoop, this, false);
  stderrThread = std::thread(&LocalHostProcess::captureLoop, this, true);
}

LocalHostProcess::~LocalHostProcess() {
  try {
    kill();
    reapIfNeeded(true);
  } catch (...) {
  }

  joinCaptureThreads();

#if defined(_WIN32)
  if (impl->stdoutRead) {
    CloseHandle(impl->stdoutRead);
    impl->stdoutRead = nullptr;
  }
  if (impl->stderrRead) {
    CloseHandle(impl->stderrRead);
    impl->stderrRead = nullptr;
  }
  if (impl->stdinWrite) {
    CloseHandle(impl->stdinWrite);
    impl->stdinWrite = nullptr;
  }
  if (impl->threadHandle) {
    CloseHandle(impl->threadHandle);
    impl->threadHandle = nullptr;
  }
  if (impl->processHandle) {
    CloseHandle(impl->processHandle);
    impl->processHandle = nullptr;
  }
#else
  if (impl->stdoutFd != -1) {
    close(impl->stdoutFd);
    impl->stdoutFd = -1;
  }
  if (impl->stderrFd != -1) {
    close(impl->stderrFd);
    impl->stderrFd = -1;
  }
  if (impl->stdinFd != -1) {
    close(impl->stdinFd);
    impl->stdinFd = -1;
  }
#endif
}

void LocalHostProcess::onOutput(
    std::function<void(const std::string &, bool isError)> cb) {
  std::lock_guard<std::mutex> lock(callbackMutex);
  callback = std::move(cb);
}

shared::ProcessResult LocalHostProcess::wait() {
  reapIfNeeded(true);
  joinCaptureThreads();

  auto end = std::chrono::steady_clock::now();
  double duration =
      std::chrono::duration<double, std::milli>(end - startTime).count();

  shared::ProcessResult res;
  res.exitCode = exitCode.load();
  {
    std::lock_guard<std::mutex> lock(callbackMutex);
    res.stdoutData = stdoutBuffer;
    res.stderrData = stderrBuffer;
  }
  res.durationMs = duration;
  res.finishReason = shared::ProcessFinishReason::Natural;
  return res;
}

shared::ProcessSnapshot LocalHostProcess::inspect() const {
  auto *nonConstThis = const_cast<LocalHostProcess *>(this);
  if (nonConstThis->reapIfNeeded(false)) {
    nonConstThis->joinCaptureThreads();
  }

  std::string stdoutData;
  std::string stderrData;
  {
    std::lock_guard<std::mutex> lock(callbackMutex);
    stdoutData = stdoutBuffer;
    stderrData = stderrBuffer;
  }
  auto now = std::chrono::steady_clock::now();
  double elapsed =
      std::chrono::duration<double, std::milli>(now - startTime).count();

  return {!finished.load(), exitCode.load(), stdoutData, stderrData, elapsed,
          getSystemId()};
}

void LocalHostProcess::kill() {
  reapIfNeeded(false);
  if (finished.load()) {
    return;
  }
#if defined(_WIN32)
  if (!TerminateProcess(impl->processHandle, 1)) {
    throw std::runtime_error("TerminateProcess failed");
  }
#else
  const pid_t processGroupId =
      impl->processGroupId > 0 ? impl->processGroupId : impl->pid;
  if (processGroupId > 0) {
    if (::kill(-processGroupId, SIGKILL) != 0 && errno != ESRCH) {
      throw std::runtime_error("killpg failed");
    }
  }
  if (::kill(impl->pid, SIGKILL) != 0 && errno != ESRCH) {
    throw std::runtime_error("kill failed");
  }
#endif
  reapIfNeeded(true);
}

void LocalHostProcess::write(const std::string &data) {
  if (finished.load()) {
    return;
  }

#if defined(_WIN32)
  if (!impl->stdinWrite) {
    return;
  }
  size_t totalWritten = 0;
  while (totalWritten < data.size()) {
    DWORD written = 0;
    if (!WriteFile(impl->stdinWrite, data.data() + totalWritten,
                   static_cast<DWORD>(data.size() - totalWritten), &written,
                   nullptr)) {
      throw std::runtime_error("Write to process failed");
    }
    if (written == 0) {
      break;
    }
    totalWritten += written;
  }
#else
  if (impl->stdinFd == -1) {
    return;
  }
  size_t totalWritten = 0;
  while (totalWritten < data.size()) {
    ssize_t res = ::write(impl->stdinFd, data.data() + totalWritten,
                          data.size() - totalWritten);
    if (res < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      throw std::runtime_error("Write to process failed");
    }
    if (res == 0) {
      break;
    }
    totalWritten += static_cast<size_t>(res);
  }
#endif
}

bool LocalHostProcess::isRunning() {
  reapIfNeeded(false);
  return !finished.load();
}

std::string LocalHostProcess::getSystemId() const {
#if defined(_WIN32)
  return std::to_string(static_cast<unsigned long>(impl->pid));
#else
  return std::to_string(impl->pid);
#endif
}

void LocalHostProcess::captureLoop(bool isError) {
  char buf[4096];

#if defined(_WIN32)
  HANDLE readHandle = isError ? impl->stderrRead : impl->stdoutRead;
  if (!readHandle) {
    return;
  }

  while (true) {
    DWORD bytes = 0;
    BOOL ok = ReadFile(readHandle, buf, sizeof(buf), &bytes, nullptr);
    if (!ok || bytes == 0) {
      break;
    }

    std::string data(buf, bytes);
    std::function<void(const std::string &, bool)> currentCallback;
    {
      std::lock_guard<std::mutex> lock(callbackMutex);
      if (isError) {
        if (stderrBuffer.size() < 10 * 1024 * 1024) {
          stderrBuffer += data;
        }
      } else {
        if (stdoutBuffer.size() < 10 * 1024 * 1024) {
          stdoutBuffer += data;
        }
      }
      currentCallback = callback;
    }
    if (currentCallback) {
      currentCallback(data, isError);
    }
  }
#else
  int &fd = isError ? impl->stderrFd : impl->stdoutFd;
  if (fd == -1) {
    return;
  }

  while (true) {
    ssize_t bytes = read(fd, buf, sizeof(buf));
    if (bytes > 0) {
      std::string data(buf, static_cast<size_t>(bytes));
      std::function<void(const std::string &, bool)> currentCallback;
      {
        std::lock_guard<std::mutex> lock(callbackMutex);
        if (isError) {
          if (stderrBuffer.size() < 10 * 1024 * 1024) {
            stderrBuffer += data;
          }
        } else {
          if (stdoutBuffer.size() < 10 * 1024 * 1024) {
            stdoutBuffer += data;
          }
        }
        currentCallback = callback;
      }
      if (currentCallback) {
        currentCallback(data, isError);
      }
      continue;
    }

    if (bytes == 0) {
      close(fd);
      fd = -1;
      break;
    }

    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    close(fd);
    fd = -1;
    break;
  }
#endif

  reapIfNeeded(false);
}

void LocalHostProcess::joinCaptureThreads() {
  if (stdoutThread.joinable()) {
    stdoutThread.join();
  }
  if (stderrThread.joinable()) {
    stderrThread.join();
  }
}

bool LocalHostProcess::reapIfNeeded(bool block) {
  if (finished.load()) {
    return true;
  }

  std::lock_guard<std::mutex> lock(stateMutex);
  if (reaped) {
    return true;
  }

#if defined(_WIN32)
  DWORD waitRes =
      WaitForSingleObject(impl->processHandle, block ? INFINITE : 0);
  if (waitRes == WAIT_TIMEOUT) {
    return false;
  }
  if (waitRes != WAIT_OBJECT_0) {
    return false;
  }

  DWORD status = 0;
  if (!GetExitCodeProcess(impl->processHandle, &status)) {
    return false;
  }

  exitCode.store(static_cast<int>(status));
  reaped = true;
  finished.store(true);
  return true;
#else
  int status = 0;
  pid_t res = waitpid(impl->pid, &status, block ? 0 : WNOHANG);
  if (res == 0) {
    return false;
  }
  if (res == impl->pid) {
    exitCode.store(decodeExitStatus(status));
    reaped = true;
    finished = true;
    return true;
  }
  if (res < 0 && errno == ECHILD) {
    reaped = true;
    finished = true;
    return true;
  }
  return false;
#endif
}

int LocalHostProcess::decodeExitStatus(int status) {
#if defined(_WIN32)
  return status;
#else
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
#endif
}

std::string LocalHost::init() { return "localhost"; }

void LocalHost::destroy() {}

void LocalHost::promoteCompletedProcessLocked(
    const std::string &id, std::unique_ptr<IHostProcess> proc,
    const shared::ProcessSnapshot &snapshot) {
  (void)proc;
  completedBackgroundProcesses[id] = {snapshot, std::chrono::steady_clock::now()};
  while (completedBackgroundProcesses.size() > kMaxCompletedBackgroundProcesses) {
    auto oldest = completedBackgroundProcesses.begin();
    for (auto it = std::next(completedBackgroundProcesses.begin());
         it != completedBackgroundProcesses.end(); ++it) {
      if (it->second.completedAt < oldest->second.completedAt) {
        oldest = it;
      }
    }
    completedBackgroundProcesses.erase(oldest);
  }
}

std::map<std::string, LocalHost::CompletedProcessSnapshot>::iterator
LocalHost::touchCompletedProcessLocked(const std::string &id) {
  auto it = completedBackgroundProcesses.find(id);
  if (it != completedBackgroundProcesses.end()) {
    it->second.completedAt = std::chrono::steady_clock::now();
  }
  return it;
}

void LocalHost::cleanup() {
  std::lock_guard<std::mutex> lock(bgMutex);
  for (auto &[id, proc] : backgroundProcesses) {
    if (proc) {
      proc->onOutput({});
      proc->kill();
    }
  }
  backgroundProcesses.clear();
  completedBackgroundProcesses.clear();
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

void LocalHost::deleteFile(const std::string &path) {
  std::error_code ec;
  // remove() returns false if the file didn't exist — treat that as
  // success (idempotent), only throw on a real I/O error. Using the
  // ec-overload prevents an exception from std::filesystem on missing
  // path so we control the failure message.
  if (!std::filesystem::remove(path, ec) && ec) {
    throw std::runtime_error("Could not delete file: " + path + ": " +
                              ec.message());
  }
}

bool LocalHost::exists(const std::string &path) {
  return std::filesystem::exists(path);
}

std::vector<shared::FileInfo> LocalHost::listDir(const std::string &path) {
  std::filesystem::path dirPath(path);
  if (!std::filesystem::exists(dirPath)) {
    throw std::runtime_error("Path not found: " + path);
  }
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
  return {fspath.filename().string(), fspath.string(), size, isDir, isSymlink,
          modMs};
}

shared::ProcessResult
LocalHost::exec(const std::string &command, const std::string &cwd,
                const std::map<std::string, std::string> &env,
                std::optional<std::chrono::milliseconds> timeout) {
  auto start = std::chrono::steady_clock::now();
  auto proc = spawn(command, cwd, env);

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
#if defined(_WIN32)
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.lpSecurityDescriptor = nullptr;
  sa.bInheritHandle = TRUE;

  HANDLE outRead = nullptr;
  HANDLE outWrite = nullptr;
  HANDLE errRead = nullptr;
  HANDLE errWrite = nullptr;
  HANDLE inRead = nullptr;
  HANDLE inWrite = nullptr;

  if (!CreatePipe(&outRead, &outWrite, &sa, 0) ||
      !CreatePipe(&errRead, &errWrite, &sa, 0) ||
      !CreatePipe(&inRead, &inWrite, &sa, 0)) {
    throw std::runtime_error("CreatePipe failed");
  }

  SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si{};
  si.cb = sizeof(STARTUPINFOA);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = inRead;
  si.hStdOutput = outWrite;
  si.hStdError = errWrite;

  PROCESS_INFORMATION pi{};

  std::string shellCommand = buildWindowsShellCommand(command);
  std::vector<char> commandLine(shellCommand.begin(), shellCommand.end());
  commandLine.push_back('\0');

  std::vector<char> envBlock = buildWindowsEnvironmentBlock(env);
  LPVOID envPtr = env.empty() ? nullptr : envBlock.data();

  BOOL ok = CreateProcessA(
      nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
      envPtr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);

  CloseHandle(outWrite);
  CloseHandle(errWrite);
  CloseHandle(inRead);

  if (!ok) {
    CloseHandle(outRead);
    CloseHandle(errRead);
    CloseHandle(inWrite);
    throw std::runtime_error("CreateProcess failed");
  }

  auto processImpl = std::make_unique<LocalHostProcess::Impl>();
  processImpl->processHandle = pi.hProcess;
  processImpl->threadHandle = pi.hThread;
  processImpl->stdoutRead = outRead;
  processImpl->stderrRead = errRead;
  processImpl->stdinWrite = inWrite;
  processImpl->pid = pi.dwProcessId;

  return std::make_unique<LocalHostProcess>(std::move(processImpl));
#else
  int outPipe[2], errPipe[2], inPipe[2];
  if (pipe(outPipe) != 0 || pipe(errPipe) != 0 || pipe(inPipe) != 0)
    throw std::runtime_error("Pipe failed");

  pid_t pid = fork();
  if (pid == 0) {
    if (setpgid(0, 0) != 0) {
      _exit(127);
    }

    if (!currentUser.empty()) {
      struct passwd *pw = getpwnam(currentUser.c_str());
      if (pw) {
        const int setuidResult = setuid(pw->pw_uid);
        (void)setuidResult;
      }
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
  }

  if (pid < 0) {
    close(inPipe[0]);
    close(inPipe[1]);
    close(outPipe[0]);
    close(outPipe[1]);
    close(errPipe[0]);
    close(errPipe[1]);
    throw std::runtime_error("Fork failed");
  }

  close(inPipe[0]);
  close(outPipe[1]);
  close(errPipe[1]);

  (void)setpgid(pid, pid);

  auto processImpl = std::make_unique<LocalHostProcess::Impl>();
  processImpl->pid = pid;
  processImpl->processGroupId = pid;
  processImpl->stdoutFd = outPipe[0];
  processImpl->stderrFd = errPipe[0];
  processImpl->stdinFd = inPipe[1];

  return std::make_unique<LocalHostProcess>(std::move(processImpl));
#endif
}

void LocalHost::registerBackgroundProcess(const std::string &id,
                                          std::unique_ptr<IHostProcess> proc) {
  std::lock_guard<std::mutex> lock(bgMutex);
  completedBackgroundProcesses.erase(id);
  backgroundProcesses[id] = std::move(proc);
}

shared::ProcessSnapshot
LocalHost::inspectBackgroundProcess(const std::string &id) {
  std::lock_guard<std::mutex> lock(bgMutex);
  auto it = backgroundProcesses.find(id);
  if (it != backgroundProcesses.end()) {
    auto snapshot = it->second->inspect();
    if (!snapshot.running) {
      auto proc = std::move(it->second);
      backgroundProcesses.erase(it);
      promoteCompletedProcessLocked(id, std::move(proc), snapshot);
    }
    return snapshot;
  }
  auto completedIt = touchCompletedProcessLocked(id);
  if (completedIt == completedBackgroundProcesses.end()) {
    throw std::runtime_error("Background process not found: " + id);
  }
  return completedIt->second.snapshot;
}

void LocalHost::releaseBackgroundProcess(const std::string &id) {
  std::lock_guard<std::mutex> lock(bgMutex);
  auto it = backgroundProcesses.find(id);
  if (it != backgroundProcesses.end()) {
    auto snapshot = it->second->inspect();
    auto proc = std::move(it->second);
    backgroundProcesses.erase(it);
    promoteCompletedProcessLocked(id, std::move(proc), snapshot);
    return;
  }
  auto completedIt = completedBackgroundProcesses.find(id);
  if (completedIt != completedBackgroundProcesses.end()) {
    completedIt->second.completedAt = std::chrono::steady_clock::now();
  }
}

void LocalHost::writeToBackgroundProcess(const std::string &id,
                                         const std::string &data) {
  std::lock_guard<std::mutex> lock(bgMutex);
  auto it = backgroundProcesses.find(id);
  if (it == backgroundProcesses.end()) {
    throw std::runtime_error("Background process not found: " + id);
  }
  auto snapshot = it->second->inspect();
  if (!snapshot.running) {
    auto proc = std::move(it->second);
    backgroundProcesses.erase(it);
    promoteCompletedProcessLocked(id, std::move(proc), snapshot);
    throw std::runtime_error("Background process not found: " + id);
  }
  it->second->write(data);
}

void LocalHost::killBackgroundProcess(const std::string &id) {
  std::lock_guard<std::mutex> lock(bgMutex);
  auto it = backgroundProcesses.find(id);
  if (it != backgroundProcesses.end()) {
    it->second->onOutput({});
    it->second->kill();
    auto snapshot = it->second->inspect();
    auto proc = std::move(it->second);
    backgroundProcesses.erase(it);
    promoteCompletedProcessLocked(id, std::move(proc), snapshot);
  } else {
    auto completedIt = completedBackgroundProcesses.find(id);
    if (completedIt == completedBackgroundProcesses.end()) {
      throw std::runtime_error("Background process not found: " + id);
    }
    completedIt->second.completedAt = std::chrono::steady_clock::now();
  }
}

} // namespace firmius::core
