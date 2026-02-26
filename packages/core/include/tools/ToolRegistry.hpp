#ifndef FIRMIUS_CORE_TOOL_REGISTRY_HPP
#define FIRMIUS_CORE_TOOL_REGISTRY_HPP

#include "ITool.hpp"
#include "IProvider.hpp"
#include <map>
#include <memory>
#include <vector>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Registry and orchestrator for agentic tools.
 * Handles registration, discovery, and secure execution of tools.
 */
class ToolRegistry {
public:
    /**
     * @brief Registers a new tool in the registry.
     * @param tool Unique pointer to the tool instance.
     */
    void registerTool(std::unique_ptr<shared::ITool> tool);

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

private:
    std::map<std::string, std::unique_ptr<shared::ITool>> tools;
};

}

#endif
