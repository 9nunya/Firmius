#include "tools/McpGetPromptTool.hpp"

#include "ConfigLoader.hpp"
#include "IAgent.hpp"
#include "tools/McpToolUtil.hpp"

#include <algorithm>

namespace firmius::core {

shared::ToolMetadata McpGetPromptTool::getMetadata() const {
  return {"mcp_get_prompt",
          "Get a prompt from an MCP server (must be loaded first).",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> McpGetPromptTool::getSchema() const {
  return shared::zObject({
             {"server_name", shared::zString()->describe("Configured MCP server name")},
             {"prompt_name", shared::zString()->describe("Loaded MCP prompt name")},
             {"arguments",
              shared::zObject({})->describe("Optional prompt arguments")->setOptional()},
             {"timeout_ms",
              shared::zInteger()
                  ->describe("Timeout in milliseconds (default 30000)")
                  ->setOptional()},
         })
      ->required({"server_name", "prompt_name"});
}

McpGetPromptInput McpGetPromptTool::transform(const rapidjson::Value &json) {
  McpGetPromptInput input;
  MAP_STRING(server_name, "server_name");
  MAP_STRING(prompt_name, "prompt_name");
  MAP_INT(timeout_ms, "timeout_ms");

  input.arguments.SetObject();
  if (json.HasMember("arguments") && json["arguments"].IsObject()) {
    input.arguments.CopyFrom(json["arguments"], input.arguments.GetAllocator());
  }

  return input;
}

shared::ToolResult McpGetPromptTool::execute(const McpGetPromptInput &input,
                                             shared::ToolContext &ctx) {
  try {
    if (input.server_name.empty()) {
      return shared::ToolResult::fail("server_name must not be empty");
    }
    if (input.prompt_name.empty()) {
      return shared::ToolResult::fail("prompt_name must not be empty");
    }

    const auto &state = ctx.agent.getContext().state;
    const auto serverIt = std::find(state.loadedMcpServers.begin(),
                                    state.loadedMcpServers.end(),
                                    input.server_name);
    if (serverIt == state.loadedMcpServers.end()) {
      return shared::ToolResult::fail("MCP server is not loaded: " +
                                      input.server_name);
    }

    const auto loadedPromptsIt = state.loadedMcpPrompts.find(input.server_name);
    if (loadedPromptsIt == state.loadedMcpPrompts.end() ||
        std::find(loadedPromptsIt->second.begin(), loadedPromptsIt->second.end(),
                  input.prompt_name) == loadedPromptsIt->second.end()) {
      return shared::ToolResult::fail(
          "MCP prompt is not loaded for server '" + input.server_name + "': " +
          input.prompt_name);
    }

    const auto &config = shared::ConfigLoader::instance().getConfig();
    const auto cfgIt = config.mcpServers.find(input.server_name);
    if (cfgIt == config.mcpServers.end()) {
      return shared::ToolResult::fail("Unknown MCP server: " + input.server_name);
    }

    const auto &server = cfgIt->second;
    if (!server.enabled) {
      return shared::ToolResult::fail("MCP server is disabled: " +
                                      input.server_name);
    }

    const int timeoutMs = mcp_tools::resolveTimeout(input.timeout_ms);
    std::string error;
    auto client =
        mcp_tools::createClientForServer(input.server_name, server, ctx, error);
    if (!client) {
      return shared::ToolResult::fail(error);
    }

    client->initialize(timeoutMs);
    const rapidjson::Document getResponse =
        client->getPrompt(input.prompt_name, input.arguments, timeoutMs);

    rapidjson::Document out;
    out.SetObject();
    auto &a = out.GetAllocator();
    out.AddMember("server_name",
                  rapidjson::Value(input.server_name.c_str(), a).Move(), a);
    out.AddMember("prompt_name",
                  rapidjson::Value(input.prompt_name.c_str(), a).Move(), a);

    rapidjson::Value remoteResult;
    remoteResult.CopyFrom(getResponse["result"], a);
    out.AddMember("remote_result", remoteResult, a);

    return shared::ToolResult::ok(out);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
