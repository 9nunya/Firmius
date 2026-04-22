#ifndef FIRMIUS_CORE_MCP_TOOL_UTIL_HPP
#define FIRMIUS_CORE_MCP_TOOL_UTIL_HPP

#include "ConfigLoader.hpp"
#include "ITool.hpp"
#include "mcp/McpClient.hpp"
#include "utils/StringUtil.hpp"

#include <memory>
#include <string>

namespace firmius::core::mcp_tools {

constexpr int kDefaultTimeoutMs = 30000;

inline int resolveTimeout(const int timeoutMs) {
  return timeoutMs > 0 ? timeoutMs : kDefaultTimeoutMs;
}

inline std::string composeCommand(const shared::McpServerConfig &server) {
  std::string command = shared::StringUtil::shellEscape(server.command);
  for (const auto &arg : server.args) {
    command += " ";
    command += shared::StringUtil::shellEscape(arg);
  }
  return command;
}

inline std::unique_ptr<mcp::McpClient>
createClientForServer(const std::string &serverName,
                      const shared::McpServerConfig &server,
                      shared::ToolContext &ctx, std::string &error) {
  if (server.transport == "stdio") {
    if (shared::StringUtil::trim(server.command).empty()) {
      error = "MCP server command is empty: " + serverName;
      return nullptr;
    }

    const std::string command = composeCommand(server);
    auto process = ctx.host.spawn(command, server.cwd, server.env);
    if (!process) {
      error = "Failed to spawn MCP server process: " + serverName;
      return nullptr;
    }

    return std::make_unique<mcp::McpClient>(std::move(process));
  }

  if (server.transport == "http") {
    if (shared::StringUtil::trim(server.url).empty()) {
      error = "MCP server URL is empty: " + serverName;
      return nullptr;
    }

    mcp::McpHttpTransportConfig httpConfig;
    httpConfig.url = server.url;
    httpConfig.authHeader = server.authHeader;
    httpConfig.authBearerToken = server.authBearerToken;
    httpConfig.allowInsecureTls = server.allowInsecureTls;
    httpConfig.caCertPath = server.caCertPath;
    return std::make_unique<mcp::McpClient>(httpConfig);
  }

  error = "Unsupported MCP transport for server '" + serverName + "': " +
          server.transport;
  return nullptr;
}

} // namespace firmius::core::mcp_tools

#endif
