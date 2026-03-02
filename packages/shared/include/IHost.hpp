#ifndef FIRMIUS_CORE_IHOST_HPP
#define FIRMIUS_CORE_IHOST_HPP

#include "IHostProcess.hpp"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <cstdint>
#include <optional>
#include <chrono>

/**
 * @brief Host and sandbox abstraction.
 */
namespace firmius::shared {

/**
 * @brief Metadata about a filesystem entry.
 */
struct FileInfo {
    std::string name;            ///< Filename component only.
    std::string path;            ///< Full absolute path.
    uint64_t size = 0;           ///< Size in bytes.
    bool isDirectory = false;
    bool isSymlink = false;
    int64_t modifiedMs = 0;      ///< Modification time (unix epoch milliseconds).
};

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
     * @brief Perform full cleanup of all resources/containers created by this host.
     */
    virtual void cleanup() = 0;

    /**
     * @brief Set the user context for command execution.
     * @param user Username (e.g., "root", "jack").
     */
    virtual void setUser(const std::string& user) = 0;

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
     * @brief Lists entries in a directory.
     * @param path Absolute path to the directory.
     * @return Vector of FileInfo for each entry (non-recursive).
     * @throws std::runtime_error if path is not a directory or access fails.
     */
    virtual std::vector<FileInfo> listDir(const std::string& path) = 0;

    /**
     * @brief Gets metadata about a single file or directory.
     * @param path Absolute path.
     * @return FileInfo for the entry.
     * @throws std::runtime_error if path does not exist or access fails.
     */
    virtual FileInfo stat(const std::string& path) = 0;

    /**
     * @brief Executes a command synchronously (blocking).
     * @param command The shell command string.
     * @param cwd Optional working directory.
     * @param env Optional environment variables.
     * @param timeout Optional timeout for the execution.
     * @return Result of the execution (stdout, stderr, exit code).
     */
    virtual ProcessResult exec(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}, std::optional<std::chrono::milliseconds> timeout = std::nullopt) = 0;

    /**
     * @brief Spawns a command asynchronously (non-blocking).
     * @param command The shell command string.
     * @param cwd Optional working directory.
     * @param env Optional environment variables.
     * @return A handle to the spawned process.
     */
    virtual std::unique_ptr<IHostProcess> spawn(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) = 0;

    /**
     * @brief Registers a process in the host's background process tracking.
     * @param proc The process handle to register.
     * @return A unique process ID.
     */
    virtual std::string registerBackgroundProcess(std::unique_ptr<IHostProcess> proc) = 0;

    /**
     * @brief Inspects a background process.
     * @param id The process ID.
     * @return A snapshot of the process state.
     */
    virtual ProcessSnapshot inspectBackgroundProcess(const std::string& id) = 0;

    /**
     * @brief Writes data to a background process's stdin.
     * @param id The process ID.
     * @param data The data to write.
     */
    virtual void writeToBackgroundProcess(const std::string& id, const std::string& data) = 0;

    /**
     * @brief Forcefully kills a background process.
     * @param id The process ID.
     */
    virtual void killBackgroundProcess(const std::string& id) = 0;
};

}

#endif
