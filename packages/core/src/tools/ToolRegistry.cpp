#include "tools/ToolRegistry.hpp"
#include "agents/Agent.hpp"
#include <algorithm>

namespace firmius::core {
using namespace firmius::shared;

void ToolRegistry::registerTool(std::unique_ptr<shared::ITool> tool) {
    auto meta = tool->getMetadata();
    tools[meta.name] = std::move(tool);
}

std::vector<shared::ToolMetadata> ToolRegistry::listToolMetadata() const {
    std::vector<shared::ToolMetadata> metas;
    for (const auto& [name, tool] : tools) {
        metas.push_back(tool->getMetadata());
    }
    return metas;
}

std::vector<firmius::provider::ToolDefinition> ToolRegistry::getAvailableToolDefinitions(const AgentPermissions& perms) const {
    std::vector<firmius::provider::ToolDefinition> defs;
    auto& allowed = perms.allowedScopes;
    for (const auto& [name, tool] : tools) {
        auto meta = tool->getMetadata();
        if (std::find(allowed.begin(), allowed.end(), meta.scope) != allowed.end()) {
            defs.push_back({meta.name, meta.description, tool->getSchema()->toString()});
        }
    }
    return defs;
}

std::string ToolRegistry::getSchema(const std::string& name) const {
    auto it = tools.find(name);
    if (it != tools.end()) {
        return it->second->getSchema()->toString();
    }
    return "";
}

shared::ToolResult ToolRegistry::execute(const std::string& name, const rapidjson::Value& input, shared::ToolContext& ctx) {
    auto it = tools.find(name);
    if (it == tools.end()) {
        return shared::ToolResult::fail("Tool not found: " + name);
    }

    const auto& meta = it->second->getMetadata();
    
    // Security check
    auto& perms = ctx.agent.getContext().permissions;
    if (std::find(perms.allowedScopes.begin(), perms.allowedScopes.end(), meta.scope) == perms.allowedScopes.end()) {
        return shared::ToolResult::fail("Permission denied: tool scope not allowed for " + name);
    }

    // Validation with breadcrumbs
    auto validation = it->second->getSchema()->validate(input);
    if (!validation.success) {
        return shared::ToolResult::fail(validation.violationToPretty());
    }

    return it->second->execute(input, ctx);
}

}
