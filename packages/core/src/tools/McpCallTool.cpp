#include "tools/McpCallTool.hpp"

#include "ConfigLoader.hpp"
#include "mcp/McpClient.hpp"
#include "utils/StringUtil.hpp"

namespace firmius::core {
namespace {

constexpr int kDefaultTimeoutMs = 30000;

std::string composeCommand(const shared::McpStdioServerConfig &server) {
  std::string command = shared::StringUtil::shellEscape(server.command);
  for (const auto &arg : server.args) {
    command += " ";
    command += shared::StringUtil::shellEscape(arg);
  }
  return command;
}

rapidjson::Document callRemoteTool(mcp::McpClient &client,
                                   const McpCallInput &input,
                                   const int timeoutMs) {
  const rapidjson::Document listResponse = client.listTools(timeoutMs);
  const auto &listResult = listResponse["result"];
  if (!listResult.IsObject() || !listResult.HasMember("tools") ||
      !listResult["tools"].IsArray()) {
    throw std::runtime_error("MCP tools/list result missing tools array");
  }

  bool toolFound = false;
  for (const auto &entry : listResult["tools"].GetArray()) {
    if (entry.IsObject() && entry.HasMember("name") &&
        entry["name"].IsString() &&
        input.tool_name == std::string(entry["name"].GetString())) {
      toolFound = true;
      break;
    }
  }
  if (!toolFound) {
    throw std::runtime_error("Remote MCP tool not found on server '" +
                             input.server_name + "': " + input.tool_name);
  }

  return client.callTool(input.tool_name, input.arguments, timeoutMs);
}

} // namespace

shared::ToolMetadata McpCallTool::getMetadata() const {
  return {"mcp_call",
          "Calls a configured MCP server tool by server name (stdio/http).",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> McpCallTool::getSchema() const {
  return shared::zObject({
             {"server_name",
              shared::zString()->describe("Configured MCP server name")},
             {"tool_name", shared::zString()->describe("Remote MCP tool name")},
             {"arguments",
              shared::zObject({})->describe("Arguments object for remote tool")},
             {"timeout_ms",
              shared::zInteger()
                  ->describe("Timeout in milliseconds (default 30000)")
                  ->setOptional()},
         })
      ->required({"server_name", "tool_name", "arguments"});
}

McpCallInput McpCallTool::transform(const rapidjson::Value &json) {
  McpCallInput input;
  MAP_STRING(server_name, "server_name");
  MAP_STRING(tool_name, "tool_name");
  MAP_INT(timeout_ms, "timeout_ms");

  input.arguments.SetObject();
  if (json.HasMember("arguments") && json["arguments"].IsObject()) {
    input.arguments.CopyFrom(json["arguments"], input.arguments.GetAllocator());
  }

  return input;
}

shared::ToolResult McpCallTool::execute(const McpCallInput &input,
                                        shared::ToolContext &ctx) {
  try {
    if (input.server_name.empty()) {
      return shared::ToolResult::fail("server_name must not be empty");
    }
    if (input.tool_name.empty()) {
      return shared::ToolResult::fail("tool_name must not be empty");
    }

    const int timeoutMs =
        input.timeout_ms > 0 ? input.timeout_ms : kDefaultTimeoutMs;

    const auto &config = shared::ConfigLoader::instance().getConfig();
    const auto it = config.mcpServers.find(input.server_name);
    if (it == config.mcpServers.end()) {
      return shared::ToolResult::fail("Unknown MCP server: " + input.server_name);
    }

    const auto &server = it->second;
    if (!server.enabled) {
      return shared::ToolResult::fail("MCP server is disabled: " +
                                      input.server_name);
    }

    rapidjson::Document callResponse;
    if (server.transport == "stdio") {
      if (shared::StringUtil::trim(server.command).empty()) {
        return shared::ToolResult::fail("MCP server command is empty: " +
                                        input.server_name);
      }

      const std::string command = composeCommand(server);
      auto process = ctx.host.spawn(command, server.cwd, server.env);
      if (!process) {
        return shared::ToolResult::fail("Failed to spawn MCP server process: " +
                                        input.server_name);
      }

      mcp::McpClient client(std::move(process), ctx);
      client.initialize(timeoutMs);
      callResponse = callRemoteTool(client, input, timeoutMs);
    } else if (server.transport == "http") {
      if (shared::StringUtil::trim(server.url).empty()) {
        return shared::ToolResult::fail("MCP server URL is empty: " +
                                        input.server_name);
      }

      mcp::McpHttpTransportConfig httpConfig;
      httpConfig.url = server.url;
      httpConfig.authHeader = server.authHeader;
      httpConfig.authBearerToken = server.authBearerToken;
      httpConfig.allowInsecureTls = server.allowInsecureTls;
      httpConfig.caCertPath = server.caCertPath;

      mcp::McpClient client(httpConfig, ctx);
      client.initialize(timeoutMs);
      callResponse = callRemoteTool(client, input, timeoutMs);
    } else {
      return shared::ToolResult::fail("Unsupported MCP transport for server '" +
                                      input.server_name + "': " +
                                      server.transport);
    }

    rapidjson::Document out;
    out.SetObject();
    auto &a = out.GetAllocator();
    out.AddMember("server_name",
                  rapidjson::Value(input.server_name.c_str(), a).Move(), a);
    out.AddMember("tool_name", rapidjson::Value(input.tool_name.c_str(), a).Move(),
                  a);

    rapidjson::Value remoteResult;
    remoteResult.CopyFrom(callResponse["result"], a);
    out.AddMember("remote_result", remoteResult, a);

    return shared::ToolResult::ok(out);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core