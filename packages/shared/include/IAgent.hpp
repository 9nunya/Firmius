#ifndef FIRMIUS_SHARED_IAGENT_HPP
#define FIRMIUS_SHARED_IAGENT_HPP

#include "Context.hpp"
#include "Events.hpp"
#include "IHost.hpp"
#include <string>
#include <functional>
#include <map>

namespace firmius::shared {

/**
 * @brief Interface for the Agent Engine.
 */

class IAgent {
public:
    virtual ~IAgent() = default;

    /**
     * @brief Resets the agent's history and state.
     */
    virtual void reset() = 0;

    /**
     * @brief Runs the agent on a specific task.
     * @param task The task description.
     * @param onEvent Callback for real-time stream events.
     */
    virtual void run(const std::string& task, std::function<void(const StreamEvent&)> onEvent) = 0;

    /**
     * @brief Gets the current agent context (read-only).
     * @return The agent context.
     */
    virtual const AgentContext& getContext() const = 0;

    /**
     * @brief Gets the current agent context (mutable).
     * @return The agent context.
     */
    virtual AgentContext& getMutableContext() = 0;

    /**
     * @brief Resolves a path relative to the agent's CWD.
     * @param path The path to resolve.
     * @return An absolute, normalized path within the sandbox.
     */
    virtual std::string resolvePath(const std::string& path) const = 0;

    /**
     * @brief Interrupts the current agent execution.
     */
    virtual void interrupt() = 0;

    /**
     * @brief Spawns a background process.
     * @return A unique process ID.
     */
    virtual std::string spawnProcess(const std::string& command, const std::string& cwd = "", const std::map<std::string, std::string>& env = {}) = 0;

    /**
     * @brief Inspects a background process.
     * @param id The process ID.
     * @return A snapshot of the process state.
     */
    virtual ProcessSnapshot inspectProcess(const std::string& id) = 0;

    /**
     * @brief Writes data to a background process's stdin.
     * @param id The process ID.
     * @param data The data to write.
     */
    virtual void writeToProcess(const std::string& id, const std::string& data) = 0;

    /**
     * @brief Registers a process ID for agent-owned background processes.
     * @param id The process ID to register.
     */
    virtual void registerProcessId(const std::string& id) = 0;
};

}

#endif
