#ifndef FIRMIUS_TEST_MOCK_HOST_PROCESS_HPP
#define FIRMIUS_TEST_MOCK_HOST_PROCESS_HPP

#include "IHostProcess.hpp"
#include <functional>
#include <string>
#include <vector>
#include <atomic>

namespace firmius::test {

using namespace firmius::shared;

/**
 * @brief Configuration for MockHostProcess behavior.
 */
struct MockHostProcessConfig {
    std::string systemId = "mock-process-1";
    bool running = false;
    int exitCode = 0;
    std::string stdoutData;
    std::string stderrData;
    double elapsedMs = 0.0;
    ProcessFinishReason finishReason = ProcessFinishReason::Natural;
    std::string backgroundProcessId;
};

/**
 * @brief Mock implementation of IHostProcess for unit testing.
 * 
 * Provides controllable process behavior with configurable output,
 * exit codes, and state for testing tools in isolation.
 */
class MockHostProcess : public IHostProcess {
public:
    /**
     * @brief Constructs a MockHostProcess with the given configuration.
     * @param config The configuration controlling mock behavior.
     */
    explicit MockHostProcess(const MockHostProcessConfig& config = {})
        : config_(config)
        , isRunning_(config.running)
        , outputCallback_(nullptr) {}

    /**
     * @brief Attaches a callback for real-time output monitoring.
     * @param callback The callback receiving partial data and error status.
     */
    void onOutput(std::function<void(const std::string&, bool isError)> callback) override {
        outputCallback_ = callback;
    }

    /**
     * @brief Blocks until the process completes.
     * @return Final process result.
     */
    ProcessResult wait() override {
        // Simulate process completion
        isRunning_ = false;
        
        ProcessResult result;
        result.exitCode = config_.exitCode;
        result.stdoutData = config_.stdoutData;
        result.stderrData = config_.stderrData;
        result.durationMs = config_.elapsedMs;
        result.finishReason = config_.finishReason;
        result.backgroundProcessId = config_.backgroundProcessId;
        
        return result;
    }

    /**
     * @brief Inspects the current state without blocking.
     * @return A point-in-time snapshot of the process.
     */
    ProcessSnapshot inspect() const override {
        ProcessSnapshot snapshot;
        snapshot.running = isRunning_.load();
        snapshot.exitCode = config_.exitCode;
        snapshot.stdoutData = config_.stdoutData;
        snapshot.stderrData = config_.stderrData;
        snapshot.elapsedMs = config_.elapsedMs;
        return snapshot;
    }

    /**
     * @brief Forcefully kills the process.
     */
    void kill() override {
        isRunning_ = false;
    }

    /**
     * @brief Writes data to the process's stdin.
     * @param data The data to write.
     */
    void write(const std::string& data) override {
        // Record the write for verification
        writtenData_.push_back(data);
    }

    /**
     * @brief Polls the status.
     * @return True if running.
     */
    bool isRunning() override {
        return isRunning_.load();
    }

    /**
     * @brief Gets the system identifier for the process (e.g., PID).
     */
    std::string getSystemId() const override {
        return config_.systemId;
    }

    // ========== Test Configuration Methods ==========

    /**
     * @brief Sets the process as running.
     * @param running True to mark as running.
     */
    void setRunning(bool running) {
        isRunning_ = running;
    }

    /**
     * @brief Configures the exit code returned by wait() and inspect().
     * @param exitCode The exit code to return.
     */
    void setExitCode(int exitCode) {
        config_.exitCode = exitCode;
    }

    /**
     * @brief Configures the stdout data.
     * @param data The stdout content.
     */
    void setStdout(const std::string& data) {
        config_.stdoutData = data;
    }

    /**
     * @brief Configures the stderr data.
     * @param data The stderr content.
     */
    void setStderr(const std::string& data) {
        config_.stderrData = data;
    }

    /**
     * @brief Simulates output being produced by the process.
     * @param data The output data.
     * @param isError True if stderr, false if stdout.
     */
    void simulateOutput(const std::string& data, bool isError = false) {
        if (isError) {
            config_.stderrData += data;
        } else {
            config_.stdoutData += data;
        }
        
        if (outputCallback_) {
            outputCallback_(data, isError);
        }
    }

    /**
     * @brief Gets all data written to stdin via write() calls.
     * @return Vector of written data strings.
     */
    const std::vector<std::string>& getWrittenData() const {
        return writtenData_;
    }

    /**
     * @brief Clears the record of written data.
     */
    void clearWrittenData() {
        writtenData_.clear();
    }

private:
    MockHostProcessConfig config_;
    std::atomic<bool> isRunning_;
    std::function<void(const std::string&, bool)> outputCallback_;
    std::vector<std::string> writtenData_;
};

} // namespace firmius::test

#endif // FIRMIUS_TEST_MOCK_HOST_PROCESS_HPP
