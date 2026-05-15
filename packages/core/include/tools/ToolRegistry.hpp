#ifndef FIRMIUS_CORE_TOOL_REGISTRY_HPP
#define FIRMIUS_CORE_TOOL_REGISTRY_HPP

#include "ITool.hpp"
#include "IProvider.hpp"
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
#include <functional>
#include <mutex>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Registry and orchestrator for agentic tools.
 * Handles registration, discovery, and secure execution of tools.
 * Supports lazy instantiation via factory functions.
 */
class ToolRegistry {
public:
    /**
     * @brief Factory function type for lazy tool creation.
     */
    using ToolFactory = std::function<std::unique_ptr<shared::ITool>()>;

    /**
     * @brief Registers a new tool instance directly (eager loading).
     * @param tool Unique pointer to the tool instance.
     */
    void registerTool(std::unique_ptr<shared::ITool> tool);

    /**
     * @brief Registers a tool factory for lazy instantiation.
     * @param name Tool name identifier.
     * @param factory Factory function to create the tool on-demand.
     */
    void registerToolFactory(const std::string& name, ToolFactory factory);


    /**
     * @brief Registers tool factories synthesized from workflows that declare
     * defines_tool.
     */
    void registerWorkflowDefinedTools();
    /**
     * @brief Lists metadata for all registered tools.
     * @return Vector of shared::ToolMetadata.
     */
    std::vector<shared::ToolMetadata> listToolMetadata() const;

    /**
     * @brief Gets tool definitions compatible with the LLM provider, filtered by permissions.
     * @param perms Current agent permissions.
     * @return Vector of ToolDefinition for the LLM.
     */
    std::vector<firmius::provider::ToolDefinition> getAvailableToolDefinitions(const AgentPermissions& perms) const;

    /**
     * @brief Gets the JSON Schema for a tool by name.
     * @param name Tool name.
     * @return JSON Schema string.
     */
    std::string getSchema(const std::string& name) const;

    /**
     * @brief Executes a tool by name with security and validation checks.
     * @param name Tool name to execute.
     * @param input Raw JSON input arguments.
     * @param ctx Execution context.
     * @return shared::ToolResult containing success data or error message.
     */
    shared::ToolResult execute(const std::string& name, const rapidjson::Value& input, shared::ToolContext& ctx);

    /**
     * @brief Look up a tool's static metadata by name, lazy-instantiating
     * via factory when necessary.
     * @return The tool's ToolMetadata, or std::nullopt if no static tool
     *         is registered under that name (dynamic MCP tools, etc.).
     *
     * Public read-only accessor used by the mode gate and other gating
     * layers that need to consult `ToolMetadata::scope` *before* execute
     * runs.
     */
    std::optional<shared::ToolMetadata> getMetadataFor(const std::string& name) const;

private:
    /**
     * @brief Gets or creates a tool by name (lazy-loads if factory registered).
     * @param name Tool name.
     * @return Pointer to the tool, or nullptr if not found.
     */
    shared::ITool* getTool(const std::string& name) const;

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, std::unique_ptr<shared::ITool>> tools;
    mutable std::unordered_map<std::string, ToolFactory> factories;
    /**
     * @brief Truncates tool result if it is too long.
     * @param result The result to truncate.
     * @param ctx Execution context.
     * @return The truncated result.
     */
    shared::ToolResult truncateIfNecessary(shared::ToolResult result, shared::ToolContext& ctx);


};

}

#endif
