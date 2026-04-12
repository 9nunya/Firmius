#include "tools/McpSearchTool.hpp"

#include "ConfigLoader.hpp"
#include "tools/McpToolUtil.hpp"

#include <algorithm>
#include <cctype>

namespace firmius::core {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool containsInsensitive(const std::string &haystack, const std::string &needle) {
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

} // namespace

shared::ToolMetadata McpSearchTool::getMetadata() const {
  return {"mcp_search",
          "Search MCP server capability summaries (tools/resources/prompts).",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> McpSearchTool::getSchema() const {
  return shared::zObject({
             {"query", shared::zString()->describe("Search term for tool/resource/prompt names")},
             {"server_name", shared::zString()->describe("Optional MCP server name")->setOptional()},
             {"timeout_ms", shared::zInteger()->describe("Timeout in milliseconds (default 30000)")->setOptional()},
         })
      ->required({"query"});
}

shared::ToolResult McpSearchTool::execute(const McpSearchInput &input,
                                          shared::ToolContext &ctx) {
  try {
    if (input.query.empty()) {
      return shared::ToolResult::fail("query must not be empty");
    }

    const int timeoutMs = mcp_tools::resolveTimeout(input.timeout_ms);
    const auto &config = shared::ConfigLoader::instance().getConfig();

    rapidjson::Document out;
    out.SetObject();
    auto &a = out.GetAllocator();
    out.AddMember("query", rapidjson::Value(input.query.c_str(), a).Move(), a);

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
      const rapidjson::Document resourcesResponse = client->listResources(timeoutMs);
      const rapidjson::Document promptsResponse = client->listPrompts(timeoutMs);

      rapidjson::Value matchedTools(rapidjson::kArrayType);
      const auto &toolsResult = toolsResponse["result"];
      if (toolsResult.IsObject() && toolsResult.HasMember("tools") && toolsResult["tools"].IsArray()) {
        for (const auto &tool : toolsResult["tools"].GetArray()) {
          if (tool.IsObject() && tool.HasMember("name") && tool["name"].IsString()) {
            const std::string name = tool["name"].GetString();
            std::string description;
            if (tool.HasMember("description") && tool["description"].IsString()) {
              description = tool["description"].GetString();
            }
            if (containsInsensitive(name, input.query) || containsInsensitive(description, input.query)) {
              matchedTools.PushBack(rapidjson::Value(name.c_str(), a).Move(), a);
            }
          }
        }
      }

      rapidjson::Value matchedResources(rapidjson::kArrayType);
      const auto &resourcesResult = resourcesResponse["result"];
      if (resourcesResult.IsObject() && resourcesResult.HasMember("resources") && resourcesResult["resources"].IsArray()) {
        for (const auto &resource : resourcesResult["resources"].GetArray()) {
          if (resource.IsObject() && resource.HasMember("uri") && resource["uri"].IsString()) {
            const std::string uri = resource["uri"].GetString();
            if (containsInsensitive(uri, input.query)) {
              matchedResources.PushBack(rapidjson::Value(uri.c_str(), a).Move(), a);
            }
          }
        }
      }

      rapidjson::Value matchedPrompts(rapidjson::kArrayType);
      const auto &promptsResult = promptsResponse["result"];
      if (promptsResult.IsObject() && promptsResult.HasMember("prompts") && promptsResult["prompts"].IsArray()) {
        for (const auto &prompt : promptsResult["prompts"].GetArray()) {
          if (prompt.IsObject() && prompt.HasMember("name") && prompt["name"].IsString()) {
            const std::string name = prompt["name"].GetString();
            if (containsInsensitive(name, input.query)) {
              matchedPrompts.PushBack(rapidjson::Value(name.c_str(), a).Move(), a);
            }
          }
        }
      }

      if (matchedTools.Empty() && matchedResources.Empty() && matchedPrompts.Empty()) {
        continue;
      }

      rapidjson::Value serverObj(rapidjson::kObjectType);
      serverObj.AddMember("server_name", rapidjson::Value(serverName.c_str(), a).Move(), a);
      serverObj.AddMember("tools", matchedTools, a);
      serverObj.AddMember("resources", matchedResources, a);
      serverObj.AddMember("prompts", matchedPrompts, a);
      servers.PushBack(serverObj, a);
    }

    out.AddMember("servers", servers, a);
    return shared::ToolResult::ok(out);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
