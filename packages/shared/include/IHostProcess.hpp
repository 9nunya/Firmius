#ifndef FIRMIUS_CORE_IHOST_PROCESS_HPP
#define FIRMIUS_CORE_IHOST_PROCESS_HPP

#include <string>
#include <functional>
#include <vector>
#include <cstdint>

#include "Enums.hpp"

/**
 * @brief Process handling and results.
 */
namespace firmius::shared {

/**
 * @brief Result of a synchronous command execution.
 */
struct ProcessResult {
    int exitCode = -1;         ///< OS exit code (-1 if unknown).
    std::string stdoutData;    ///< Full captured stdout stream.
    std::string stderrData;    ///< Full captured stderr stream.
    double durationMs = 0.0;   ///< Total wall-clock duration.
    ProcessFinishReason finishReason = ProcessFinishReason::Natural;
    std::string backgroundProcessId;
};

/**
 * @brief Snapshot of a running process state.
 */
struct ProcessSnapshot {
    bool running;              ///< True if still active.
    int exitCode;              ///< Current exit code (valid if running is false).
    std::string stdoutData;    ///< Stdout captured up to this point.
    std::string stderrData;    ///< Stderr captured up to this point.
    double elapsedMs;          ///< Time since spawn.
};

/**
 * @brief Handle for an asynchronous background process.
 */
class IHostProcess {
public:
    virtual ~IHostProcess() = default;

    /**
     * @brief Attaches a callback for real-time output monitoring.
     * @param callback The callback receiving partial data and error status.
     */
    virtual void onOutput(std::function<void(const std::string&, bool isError)> callback) = 0;

    /**
     * @brief Blocks until the process completes.
     * @return Final process result.
     */
    virtual ProcessResult wait() = 0;

    /**
     * @brief Inspects the current state without blocking.
     * @return A point-in-time snapshot of the process.
     */
    virtual ProcessSnapshot inspect() const = 0;

    /**
     * @brief Forcefully kills the process.
     */
    virtual void kill() = 0;

    /**
     * @brief Writes data to the process's stdin.
     * @param data The data to write.
     */
    virtual void write(const std::string& data) = 0;

    /**
     * @brief Polls the status.
     * @return True if running.
     */
    virtual bool isRunning() = 0;
};

}

#endif
