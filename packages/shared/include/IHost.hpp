#ifndef FIRMIUS_CORE_IHOST_HPP
#define FIRMIUS_CORE_IHOST_HPP

#include "IHostProcess.hpp"
#include <string>
#include <vector>
#include <memory>
#include <map>

/**
 * @brief Host and sandbox abstraction.
 */
namespace firmius::shared {

/**
 * @brief Interface for an execution environment (e.g., Local, Docker).
 */
class IHost {
public:
    virtual ~IHost() = default;

    /**
     * @brief Performs environment-specific initialization (e.g. starting container).
     */
    virtual void init() = 0;

    /**
     * @brief Performs environment-specific cleanup.
     */
    virtual void destroy() = 0;

    /**
     * @brief Safely reads a file from the host filesystem.
     * @param path Absolute path to the file.
     * @return Binary data content.
     * @throws std::runtime_error if read fails.
     */
    virtual std::vector<uint8_t> readFile(const std::string& path) = 0;

    /**
     * @brief Safely writes a file to the host filesystem.
     * @param path Absolute path to the file.
     * @param data Binary data to write.
     * @throws std::runtime_error if write fails.
     */
    virtual void writeFile(const std::string& path, const std::vector<uint8_t>& data) = 0;

    /**
     * @brief Checks if a path exists on the host.
     * @param path The path to check.
     * @return True if it exists.
     */
    virtual bool exists(const std::string& path) = 0;

    /**
     * @brief Executes a command synchronously (blocking).
     * @param command The shell command string.
     * @param cwd Optional working directory.
     * @param env Optional environment variables.
     * @return Result of the execution (stdout, stderr, exit code).
     */
    virtual ProcessResult exec(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) = 0;

    /**
     * @brief Spawns a command asynchronously (non-blocking).
     * @param command The shell command string.
     * @param cwd Optional working directory.
     * @param env Optional environment variables.
     * @return A handle to the spawned process.
     */
    virtual std::unique_ptr<IHostProcess> spawn(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) = 0;
};

}

#endif
