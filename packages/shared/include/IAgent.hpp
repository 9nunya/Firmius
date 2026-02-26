#ifndef FIRMIUS_SHARED_IAGENT_HPP
#define FIRMIUS_SHARED_IAGENT_HPP

#include "Context.hpp"
#include "Events.hpp"
#include <string>
#include <functional>

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
};

}

#endif
