#include "tools/McpReadResourceTool.hpp"

#include "ConfigLoader.hpp"
#include "IAgent.hpp"
#include "tools/McpToolUtil.hpp"
#include <algorithm>

namespace firmius::core {

shared::ToolMetadata McpReadResourceTool::getMetadata() const {
  return {"mcp_read_resource",
          "Read a resource from an MCP server (must be loaded first).",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> McpReadResourceTool::getSchema() const {
  return shared::zObject({
             {"server_name", shared::zString()->describe("Configured MCP server name")},
             {"uri", shared::zString()->describe("Loaded MCP resource URI")},
             {"timeout_ms",
              shared::zInteger()
                  ->describe("Timeout in milliseconds (default 30000)")
                  ->setOptional()},
         })
      ->required({"server_name", "uri"});
}

shared::ToolResult McpReadResourceTool::execute(const McpReadResourceInput &input,
                                                shared::ToolContext &ctx) {
  try {
    if (input.server_name.empty()) {
      return shared::ToolResult::fail("server_name must not be empty");
    }
    if (input.uri.empty()) {
      return shared::ToolResult::fail("uri must not be empty");
    }

    const auto &state = ctx.agent.getContext().state;
    const auto serverIt = std::find(state.loadedMcpServers.begin(),
                                    state.loadedMcpServers.end(),
                                    input.server_name);
    if (serverIt == state.loadedMcpServers.end()) {
      return shared::ToolResult::fail("MCP server is not loaded: " +
                                      input.server_name);
    }

    const auto loadedResourcesIt = state.loadedMcpResources.find(input.server_name);
    if (loadedResourcesIt == state.loadedMcpResources.end() ||
        std::find(loadedResourcesIt->second.begin(), loadedResourcesIt->second.end(),
                  input.uri) == loadedResourcesIt->second.end()) {
      return shared::ToolResult::fail(
          "MCP resource is not loaded for server '" + input.server_name + "': " +
          input.uri);
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
    const rapidjson::Document readResponse =
        client->readResource(input.uri, timeoutMs);

    rapidjson::Document out;
    out.SetObject();
    auto &a = out.GetAllocator();
    out.AddMember("server_name",
                  rapidjson::Value(input.server_name.c_str(), a).Move(), a);
    out.AddMember("uri", rapidjson::Value(input.uri.c_str(), a).Move(), a);

    rapidjson::Value remoteResult;
    remoteResult.CopyFrom(readResponse["result"], a);
    out.AddMember("remote_result", remoteResult, a);

    return shared::ToolResult::ok(out);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
