#include "hosts/LocalHost.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <signal.h>

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
}

LocalHostProcess::LocalHostProcess(pid_t pid, int stdoutFd, int stderrFd)
    : pid(pid), stdoutFd(stdoutFd), stderrFd(stderrFd) {
    startTime = std::chrono::steady_clock::now();
    setNonBlocking(stdoutFd);
    setNonBlocking(stderrFd);
    captureThread = std::thread(&LocalHostProcess::captureLoop, this);
}

LocalHostProcess::~LocalHostProcess() {
    if (captureThread.joinable()) {
        captureThread.join();
    }
    if (stdoutFd != -1) close(stdoutFd);
    if (stderrFd != -1) close(stderrFd);
}

void LocalHostProcess::onOutput(std::function<void(const std::string&, bool isError)> cb) {
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
    double duration = std::chrono::duration<double, std::milli>(end - startTime).count();

    return {exitCode, stdoutBuffer, stderrBuffer, duration};
}

shared::ProcessSnapshot LocalHostProcess::inspect() const {
    std::lock_guard<std::mutex> lock(callbackMutex);
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(now - startTime).count();
    
    return {
        !finished.load(),
        exitCode,
        stdoutBuffer,
        stderrBuffer,
        elapsed
    };
}

void LocalHostProcess::kill() {
    ::kill(pid, SIGKILL);
}

bool LocalHostProcess::isRunning() {
    if (finished) return false;
    int status;
    pid_t res = waitpid(pid, &status, WNOHANG);
    if (res == 0) return true;
    if (res == pid) {
        exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        finished = true;
        return false;
    }
    return false;
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
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd == -1) continue;
            
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t bytes = read(fds[i].fd, buf, sizeof(buf));
                if (bytes > 0) {
                    std::string data(buf, bytes);
                    {
                        std::lock_guard<std::mutex> lock(callbackMutex);
                        if (i == 0) stdoutBuffer += data;
                        else stderrBuffer += data;
                        if (callback) callback(data, i == 1);
                    }
                } else if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close(fds[i].fd);
                    if (i == 0) stdoutFd = -1; else stderrFd = -1;
                    fds[i].fd = -1;
                }
            }
        }
    }
    finished = true;
}

void LocalHost::init() {}
void LocalHost::destroy() {}

std::vector<uint8_t> LocalHost::readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Could not open file: " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void LocalHost::writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Could not open file for writing: " + path);
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
}

bool LocalHost::exists(const std::string& path) {
    return std::filesystem::exists(path);
}

shared::ProcessResult LocalHost::exec(const std::string& command, const std::string& cwd, const std::map<std::string, std::string>& env) {
    auto start = std::chrono::steady_clock::now();
    auto proc = spawn(command, cwd, env);
    auto res = proc->wait();
    auto end = std::chrono::steady_clock::now();
    res.durationMs = std::chrono::duration<double, std::milli>(end - start).count();
    return res;
}

std::unique_ptr<shared::IHostProcess> LocalHost::spawn(const std::string& command, const std::string& cwd, const std::map<std::string, std::string>& env) {
    int outPipe[2], errPipe[2];
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) throw std::runtime_error("Pipe failed");

    pid_t pid = fork();
    if (pid == 0) {
        if (!cwd.empty()) {
            std::error_code ec;
            std::filesystem::current_path(cwd, ec);
            if (ec) _exit(127);
        }
        
        for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);

        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);

        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    } else if (pid > 0) {
        close(outPipe[1]);
        close(errPipe[1]);
        return std::make_unique<LocalHostProcess>(pid, outPipe[0], errPipe[0]);
    } else {
        throw std::runtime_error("Fork failed");
    }
}

}
