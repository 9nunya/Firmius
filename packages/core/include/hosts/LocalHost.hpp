#ifndef FIRMIUS_CORE_LOCAL_HOST_HPP
#define FIRMIUS_CORE_LOCAL_HOST_HPP

#include "IHost.hpp"
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

namespace firmius::core {
using namespace firmius::shared;

using namespace firmius::shared;

/**
 * @brief Handle for a process running on the local machine.
 */
class LocalHostProcess : public shared::IHostProcess {
public:
    LocalHostProcess(pid_t pid, int stdoutFd, int stderrFd);
    ~LocalHostProcess() override;

    void onOutput(std::function<void(const std::string&, bool isError)> callback) override;
    shared::ProcessResult wait() override;
    shared::ProcessSnapshot inspect() const override;
    void kill() override;
    bool isRunning() override;

private:
    void captureLoop();

    pid_t pid;
    int stdoutFd;
    int stderrFd;
    std::function<void(const std::string&, bool isError)> callback;
    mutable std::mutex callbackMutex;
    mutable std::string stdoutBuffer;
    mutable std::string stderrBuffer;
    std::thread captureThread;
    std::atomic<bool> finished{false};
    int exitCode = -1;
    std::chrono::steady_clock::time_point startTime;
};

/**
 * @brief Host implementation for local execution.
 */
class LocalHost : public shared::IHost {
public:
    void init() override;
    void destroy() override;

    std::vector<uint8_t> readFile(const std::string& path) override;
    void writeFile(const std::string& path, const std::vector<uint8_t>& data) override;
    bool exists(const std::string& path) override;

    shared::ProcessResult exec(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) override;
    std::unique_ptr<shared::IHostProcess> spawn(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) override;
};

}

#endif
