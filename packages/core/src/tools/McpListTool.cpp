#include "tools/McpListTool.hpp"

#include "ConfigLoader.hpp"
#include "tools/McpToolUtil.hpp"
#include <algorithm>
#include <optional>

namespace firmius::core {
namespace {
constexpr int kOptionalCapabilityTimeoutMs = 1000;
}

shared::ToolMetadata McpListTool::getMetadata() const {
  return {"mcp_list",
          "List configured MCP server capabilities (tools/resources/prompts) as summaries.",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> McpListTool::getSchema() const {
  return shared::zObject({
      {"server_name", shared::zString()->describe("Optional MCP server name")->setOptional()},
      {"timeout_ms", shared::zInteger()->describe("Timeout in milliseconds (default 30000)")->setOptional()},
  });
}

shared::ToolResult McpListTool::execute(const McpListInput &input,
                                        shared::ToolContext &ctx) {
  try {
    const int timeoutMs = mcp_tools::resolveTimeout(input.timeout_ms);
    const auto &config = shared::ConfigLoader::instance().getConfig();

    rapidjson::Document out;
    out.SetObject();
    auto &a = out.GetAllocator();
    rapidjson::Value servers(rapidjson::kArrayType);

    for (const auto &[serverName, server] : config.mcpServers) {
      if (!input.server_name.empty() && input.server_name != serverName) {
        continue;
      }
      if (!server.enabled) {
        continue;
      }

      std::string error;
      auto client = mcp_tools::createClientForServer(serverName, server, ctx, error);
      if (!client) {
        return shared::ToolResult::fail(error);
      }

      client->initialize(timeoutMs);
      const rapidjson::Document toolsResponse = client->listTools(timeoutMs);

      std::optional<rapidjson::Document> resourcesResponse;
      try {
        resourcesResponse.emplace(
            client->listResources(std::min(timeoutMs, kOptionalCapabilityTimeoutMs)));
      } catch (...) {
      }

      std::optional<rapidjson::Document> promptsResponse;
      try {
        promptsResponse.emplace(
            client->listPrompts(std::min(timeoutMs, kOptionalCapabilityTimeoutMs)));
      } catch (...) {
      }

      rapidjson::Value serverObj(rapidjson::kObjectType);
      serverObj.AddMember("server_name", rapidjson::Value(serverName.c_str(), a).Move(), a);

      rapidjson::Value toolNames(rapidjson::kArrayType);
      const auto &toolsResult = toolsResponse["result"];
      if (toolsResult.IsObject() && toolsResult.HasMember("tools") &&
          toolsResult["tools"].IsArray()) {
        for (const auto &tool : toolsResult["tools"].GetArray()) {
          if (tool.IsObject() && tool.HasMember("name") && tool["name"].IsString()) {
            toolNames.PushBack(rapidjson::Value(tool["name"].GetString(), a).Move(), a);
          }
        }
      }
      serverObj.AddMember("tools", toolNames, a);

      rapidjson::Value resourceUris(rapidjson::kArrayType);
      if (resourcesResponse.has_value()) {
        const auto &resourcesResult = (*resourcesResponse)["result"];
        if (resourcesResult.IsObject() && resourcesResult.HasMember("resources") &&
            resourcesResult["resources"].IsArray()) {
          for (const auto &resource : resourcesResult["resources"].GetArray()) {
            if (resource.IsObject() && resource.HasMember("uri") &&
                resource["uri"].IsString()) {
              resourceUris.PushBack(
                  rapidjson::Value(resource["uri"].GetString(), a).Move(), a);
            }
          }
        }
      }
      serverObj.AddMember("resources", resourceUris, a);

      rapidjson::Value promptNames(rapidjson::kArrayType);
      if (promptsResponse.has_value()) {
        const auto &promptsResult = (*promptsResponse)["result"];
        if (promptsResult.IsObject() && promptsResult.HasMember("prompts") &&
            promptsResult["prompts"].IsArray()) {
          for (const auto &prompt : promptsResult["prompts"].GetArray()) {
            if (prompt.IsObject() && prompt.HasMember("name") &&
                prompt["name"].IsString()) {
              promptNames.PushBack(
                  rapidjson::Value(prompt["name"].GetString(), a).Move(), a);
            }
          }
        }
      }
      serverObj.AddMember("prompts", promptNames, a);

      servers.PushBack(serverObj, a);
    }

    out.AddMember("servers", servers, a);
    return shared::ToolResult::ok(out);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
