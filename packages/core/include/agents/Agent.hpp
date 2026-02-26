#ifndef FIRMIUS_CORE_AGENT_HPP
#define FIRMIUS_CORE_AGENT_HPP

#include "IAgent.hpp"
#include "IProvider.hpp"
#include "IHost.hpp"
#include "tools/ToolRegistry.hpp"
#include "Events.hpp"

#include <functional>
#include <string>
#include <vector>
#include <memory>

namespace firmius::core {
using namespace firmius::shared;

using namespace firmius::shared;

/**
 * @brief The primary Agent Engine implementation.
 * Encapsulates the agent's context, provider interaction, and tool execution loop.
 */
class Agent : public IAgent {
public:
    /**
     * @brief Constructs an Agent.
     * @param context The initial agent universe/context.
     * @param provider The LLM provider for reasoning and generation.
     * @param host The execution environment (Local or Docker).
     * @param toolRegistry The registry of available tools.
     */
    Agent(AgentContext context, firmius::provider::IProvider& provider, shared::IHost& host, ToolRegistry& toolRegistry);

    /**
     * @brief Resets the agent's history.
     */
    void reset() override;

    /**
     * @brief Runs the autonomous agent loop to solve a task.
     * @param task The user-provided task description.
     * @param onEvent Callback for real-time streaming events.
     */
    void run(const std::string& task, std::function<void(const StreamEvent&)> onEvent) override;

    /**
     * @brief Gets the current agent context (read-only).
     */
    const AgentContext& getContext() const override { return context; }

    /**
     * @brief Gets the current agent context (mutable).
     */
    AgentContext& getMutableContext() override { return context; }

    /**
     * @brief Resolves a path relative to the agent's current working directory.
     * @param inputPath The path provided by the LLM.
     * @return An absolute, normalized path within the sandbox.
     */
    std::string resolvePath(const std::string& inputPath) const override;

private:
    /**
     * @brief Internal helper to execute a batch of tool calls.
     * @param chunks The tool call deltas received from the provider.
     * @param onEvent Callback to notify the caller of tool execution events.
     */
    void executeTools(const std::vector<ToolCallChunk>& chunks, std::function<void(const StreamEvent&)> onEvent);

    AgentContext context;
    firmius::provider::IProvider& provider;
    shared::IHost& host;
    ToolRegistry& toolRegistry;
    bool debugPrettyPrint = false;
};

}

#endif
