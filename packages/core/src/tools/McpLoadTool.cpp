#include "tools/McpLoadTool.hpp"

#include "ConfigLoader.hpp"
#include "tools/McpToolUtil.hpp"

#include <algorithm>
#include <optional>

namespace firmius::core {
namespace {

std::vector<std::string> parseStringArray(const rapidjson::Value &json,
                                          const char *field) {
  std::vector<std::string> out;
  if (!json.HasMember(field) || !json[field].IsArray()) {
    return out;
  }

  for (const auto &entry : json[field].GetArray()) {
    if (entry.IsString()) {
      out.push_back(entry.GetString());
    }
  }
  return out;
}

bool contains(const std::vector<std::string> &values, const std::string &value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

shared::ToolMetadata McpLoadTool::getMetadata() const {
  return {"mcp_load",
          "Select and persist MCP tools/resources/prompts for a server.",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> McpLoadTool::getSchema() const {
  return shared::zObject({
             {"server_name", shared::zString()->describe("Configured MCP server name")},
             {"tools", shared::zArray(shared::zString())->describe("Tool names to load")->setOptional()},
             {"resources", shared::zArray(shared::zString())->describe("Resource URIs to load")->setOptional()},
             {"prompts", shared::zArray(shared::zString())->describe("Prompt names to load")->setOptional()},
             {"timeout_ms", shared::zInteger()->describe("Timeout in milliseconds (default 30000)")->setOptional()},
         })
      ->required({"server_name"});
}

McpLoadInput McpLoadTool::transform(const rapidjson::Value &json) {
  McpLoadInput input;
  MAP_STRING(server_name, "server_name");
  MAP_INT(timeout_ms, "timeout_ms");
  input.tools = parseStringArray(json, "tools");
  input.resources = parseStringArray(json, "resources");
  input.prompts = parseStringArray(json, "prompts");
  return input;
}

shared::ToolResult McpLoadTool::execute(const McpLoadInput &input,
                                        shared::ToolContext &ctx) {
  try {
    if (input.server_name.empty()) {
      return shared::ToolResult::fail("server_name must not be empty");
    }

    if (input.tools.empty() && input.resources.empty() && input.prompts.empty()) {
      return shared::ToolResult::fail("At least one of tools/resources/prompts must be provided");
    }

    const int timeoutMs = mcp_tools::resolveTimeout(input.timeout_ms);
    const auto &config = shared::ConfigLoader::instance().getConfig();
    const auto it = config.mcpServers.find(input.server_name);
    if (it == config.mcpServers.end()) {
      return shared::ToolResult::fail("Unknown MCP server: " + input.server_name);
    }

    const auto &server = it->second;
    if (!server.enabled) {
      return shared::ToolResult::fail("MCP server is disabled: " + input.server_name);
    }

    std::string error;
    auto client = mcp_tools::createClientForServer(input.server_name, server, ctx, error);
    if (!client) {
      return shared::ToolResult::fail(error);
    }

    client->initialize(timeoutMs);
    const rapidjson::Document toolsResponse = client->listTools(timeoutMs);
    std::optional<rapidjson::Document> resourcesResponse;
    if (!input.resources.empty()) {
      resourcesResponse.emplace(client->listResources(timeoutMs));
    }
    std::optional<rapidjson::Document> promptsResponse;
    if (!input.prompts.empty()) {
      promptsResponse.emplace(client->listPrompts(timeoutMs));
    }

    std::vector<std::string> availableTools;
    const auto &toolsResult = toolsResponse["result"];
    if (toolsResult.IsObject() && toolsResult.HasMember("tools") && toolsResult["tools"].IsArray()) {
      for (const auto &tool : toolsResult["tools"].GetArray()) {
        if (tool.IsObject() && tool.HasMember("name") && tool["name"].IsString()) {
          availableTools.push_back(tool["name"].GetString());
        }
      }
    }

    std::vector<std::string> availableResources;
    if (resourcesResponse.has_value()) {
      const auto &resourcesResult = (*resourcesResponse)["result"];
      if (resourcesResult.IsObject() &&
          resourcesResult.HasMember("resources") &&
          resourcesResult["resources"].IsArray()) {
        for (const auto &resource : resourcesResult["resources"].GetArray()) {
          if (resource.IsObject() && resource.HasMember("uri") &&
              resource["uri"].IsString()) {
            availableResources.push_back(resource["uri"].GetString());
          }
        }
      }
    }

    std::vector<std::string> availablePrompts;
    if (promptsResponse.has_value()) {
      const auto &promptsResult = (*promptsResponse)["result"];
      if (promptsResult.IsObject() && promptsResult.HasMember("prompts") &&
          promptsResult["prompts"].IsArray()) {
        for (const auto &prompt : promptsResult["prompts"].GetArray()) {
          if (prompt.IsObject() && prompt.HasMember("name") &&
              prompt["name"].IsString()) {
            availablePrompts.push_back(prompt["name"].GetString());
          }
        }
      }
    }

    for (const auto &tool : input.tools) {
      if (!contains(availableTools, tool)) {
        return shared::ToolResult::fail("MCP tool not found on server '" + input.server_name + "': " + tool);
      }
    }
    for (const auto &resource : input.resources) {
      if (!contains(availableResources, resource)) {
        return shared::ToolResult::fail("MCP resource not found on server '" + input.server_name + "': " + resource);
      }
    }
    for (const auto &prompt : input.prompts) {
      if (!contains(availablePrompts, prompt)) {
        return shared::ToolResult::fail("MCP prompt not found on server '" + input.server_name + "': " + prompt);
      }
    }

    rapidjson::Document out;
    out.SetObject();
    auto &a = out.GetAllocator();
    out.AddMember("server_name", rapidjson::Value(input.server_name.c_str(), a).Move(), a);

    rapidjson::Value loadedTools(rapidjson::kArrayType);
    for (const auto &tool : input.tools) {
      loadedTools.PushBack(rapidjson::Value(tool.c_str(), a).Move(), a);
    }
    out.AddMember("loaded_tools", loadedTools, a);

    rapidjson::Value loadedResources(rapidjson::kArrayType);
    for (const auto &resource : input.resources) {
      loadedResources.PushBack(rapidjson::Value(resource.c_str(), a).Move(), a);
    }
    out.AddMember("loaded_resources", loadedResources, a);

    rapidjson::Value loadedPrompts(rapidjson::kArrayType);
    for (const auto &prompt : input.prompts) {
      loadedPrompts.PushBack(rapidjson::Value(prompt.c_str(), a).Move(), a);
    }
    out.AddMember("loaded_prompts", loadedPrompts, a);

    return shared::ToolResult::ok(out);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
