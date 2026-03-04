#ifndef FIRMIUS_CORE_LOCAL_HOST_HPP
#define FIRMIUS_CORE_LOCAL_HOST_HPP

#include "IHost.hpp"
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Handle for a process running on the local machine.
 * Provides real-time output capture and state inspection.
 */
class LocalHostProcess : public shared::IHostProcess {
public:
    /**
     * @brief Constructs a LocalHostProcess.
     * @param pid OS process ID.
     * @param stdoutFd Pipe file descriptor for stdout.
     * @param stderrFd Pipe file descriptor for stderr.
     * @param stdinFd Pipe file descriptor for stdin.
     */
    LocalHostProcess(pid_t pid, int stdoutFd, int stderrFd, int stdinFd);
    ~LocalHostProcess() override;

    void onOutput(std::function<void(const std::string&, bool isError)> callback) override;
    shared::ProcessResult wait() override;
    shared::ProcessSnapshot inspect() const override;
    void kill() override;
    void write(const std::string& data) override;
    bool isRunning() override;
    std::string getSystemId() const override;

private:
    /**
     * @brief Background loop to read from pipes.
     */
    void captureLoop();

    pid_t pid;
    int stdoutFd;
    int stderrFd;
    int stdinFd;
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
 * Uses fork/exec to run commands on the native machine.
 */
class LocalHost : public shared::IHost {
public:
    std::string init() override;
    void destroy() override;
    void cleanup() override;
    void setUser(const std::string& user) override;
    std::string getId() const override { return "localhost"; }

    std::vector<uint8_t> readFile(const std::string& path) override;
    void writeFile(const std::string& path, const std::vector<uint8_t>& data) override;
    bool exists(const std::string& path) override;
    std::vector<shared::FileInfo> listDir(const std::string& path) override;
    shared::FileInfo stat(const std::string& path) override;

    shared::ProcessResult exec(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}, std::optional<std::chrono::milliseconds> timeout = std::nullopt) override;
    std::unique_ptr<shared::IHostProcess> spawn(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) override;

    void registerBackgroundProcess(const std::string& id, std::unique_ptr<shared::IHostProcess> proc) override;
    shared::ProcessSnapshot inspectBackgroundProcess(const std::string& id) override;
    void writeToBackgroundProcess(const std::string& id, const std::string& data) override;
    void killBackgroundProcess(const std::string& id) override;

private:
    std::string currentUser;
    std::map<std::string, std::unique_ptr<shared::IHostProcess>> backgroundProcesses;
    mutable std::mutex bgMutex;
};

}

#endif
