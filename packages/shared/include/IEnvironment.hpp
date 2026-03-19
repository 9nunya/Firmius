#ifndef FIRMIUS_SHARED_IENVIRONMENT_HPP
#define FIRMIUS_SHARED_IENVIRONMENT_HPP

#include "Events.hpp"
#include "IHost.hpp"
#include "IPermissions.hpp"
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace firmius::shared {

/**
 * @brief Forward declarations.
 */
class IProcessManager;
class IWorkspace;

/**
 * @brief Interface for managing background processes.
 */
class IProcessManager {
public:
    virtual ~IProcessManager() = default;

    /**
     * @brief Spawns a background process.
     * @param command The command to execute.
     * @param toolCallId Optional tool call ID for tracking.
     * @param cwd Optional working directory.
     * @param env Optional environment variables.
     * @return Unique process ID.
     */
    virtual std::string spawnProcess(
        const std::string& command,
        const std::string& toolCallId = "",
        const std::string& cwd = "",
        const std::map<std::string, std::string>& env = {},
        bool monitorCompletion = false) = 0;

    /**
     * @brief Inspects a background process.
     * @param id The process ID.
     * @return Process snapshot.
     */
    virtual ProcessSnapshot inspectProcess(const std::string& id) = 0;

    /**
     * @brief Writes data to a process's stdin.
     * @param id The process ID.
     * @param data The data to write.
     */
    virtual void writeToProcess(const std::string& id, const std::string& data) = 0;

    /**
     * @brief Registers a process ID for tracking.
     * @param id The process ID.
     */
    virtual void registerProcessId(const std::string& id) = 0;

    /**
     * @brief Emits a process spawned event.
     * @param processId The process ID.
     * @param toolCallId The tool call ID.
     * @param command The command executed.
     */
    virtual void emitProcessSpawned(
        const std::string& processId,
        const std::string& toolCallId,
        const std::string& command) = 0;

    /**
     * @brief Adds a process to the blocking list.
     * @param id The process ID.
     */
    virtual void addBlockingProcessId(const std::string& id) = 0;

    /**
     * @brief Removes a process from the blocking list.
     * @param id The process ID.
     */
    virtual void removeBlockingProcessId(const std::string& id) = 0;

    /**
     * @brief Gets all blocking process IDs.
     * @return Vector of blocking process IDs.
     */
    virtual std::vector<std::string> getBlockingProcessIds() = 0;

    /**
     * @brief Kills a background process.
     * @param id The process ID.
     */
    virtual void killProcess(const std::string& id) = 0;
};

/**
 * @brief Interface for workspace/file operations.
 */
class IWorkspace {
public:
    virtual ~IWorkspace() = default;

    /**
     * @brief Resolves a path relative to the current working directory.
     * @param path The path to resolve.
     * @return Absolute resolved path.
     */
    virtual std::string resolvePath(const std::string& path) const = 0;

    /**
     * @brief Checks if a file has been read.
     * @param path The absolute path.
     * @return True if read.
     */
    virtual bool hasReadFile(const std::string& path) const = 0;

    /**
     * @brief Marks a file as read.
     * @param path The absolute path.
     */
    virtual void markFileAsRead(const std::string& path) = 0;

    /**
     * @brief Records a line-oriented read segment for a file.
     * @param path The absolute path.
     * @param startLine The first 1-indexed line included in the read.
     * @param endLine The last 1-indexed line included in the read.
     * @param reachedEnd Whether this read segment reached EOF.
     */
    virtual void recordFileRead(const std::string& path, int startLine,
                                int endLine, bool reachedEnd) {
        (void)startLine;
        (void)endLine;
        (void)reachedEnd;
        markFileAsRead(path);
    }

    /**
     * @brief Checks if a file has been fully read.
     * @param path The absolute path.
     * @return True if fully read.
     */
    virtual bool hasFullyReadFile(const std::string& path) const = 0;

    /**
     * @brief Marks a file as fully read.
     * @param path The absolute path.
     */
    virtual void markFileAsFullyRead(const std::string& path) = 0;

    /**
     * @brief Gets the current working directory.
     * @return The CWD.
     */
    virtual std::string getCurrentWorkingDirectory() const = 0;
};

/**
 * @brief Interface for the execution environment.
 * 
 * Combines process management, workspace operations, and host access.
 * Can be shared between multiple agents via shared_ptr.
 */
class IEnvironment {
public:
    virtual ~IEnvironment() = default;

    /**
     * @brief Gets the environment identifier.
     * @return "localhost" for local, container ID for Docker.
     */
    virtual std::string getId() const = 0;

    /**
     * @brief Gets the process manager.
     * @return Reference to the process manager.
     */
    virtual IProcessManager& getProcessManager() = 0;

    /**
     * @brief Gets the workspace.
     * @return Reference to the workspace.
     */
    virtual IWorkspace& getWorkspace() = 0;

    /**
     * @brief Gets the underlying host.
     * @return Shared pointer to the host.
     */
    virtual std::shared_ptr<IHost> getHost() = 0;

    /**
     * @brief Performs cleanup of all resources.
     * Called when the last shared_ptr is destroyed.
     */
    virtual void cleanup() = 0;

    /**
     * @brief Checks if this environment is still valid/active.
     * @return True if active.
     */
    virtual bool isActive() const = 0;
};

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_IENVIRONMENT_HPP
