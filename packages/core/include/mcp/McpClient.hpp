#ifndef FIRMIUS_CORE_MCP_CLIENT_HPP
#define FIRMIUS_CORE_MCP_CLIENT_HPP

#include "ITool.hpp"
#include "mcp/IMcpSession.hpp"
#include "mcp/McpHttpSession.hpp"

#include <rapidjson/document.h>

#include <memory>
#include <string>

namespace firmius::core::mcp {

class McpClient {
public:
  McpClient(std::unique_ptr<shared::IHostProcess> process,
            shared::ToolContext &ctx);
  McpClient(const McpHttpTransportConfig &httpConfig, shared::ToolContext &ctx);
  McpClient(std::unique_ptr<IMcpSession> session, shared::ToolContext &ctx);
  ~McpClient();

  McpClient(const McpClient &) = delete;
  McpClient &operator=(const McpClient &) = delete;

  void initialize(int timeoutMs);
  rapidjson::Document listTools(int timeoutMs);
  rapidjson::Document callTool(const std::string &toolName,
                               const rapidjson::Document &arguments,
                               int timeoutMs);
  rapidjson::Document listResources(int timeoutMs);
  rapidjson::Document listPrompts(int timeoutMs);
  rapidjson::Document readResource(const std::string &resourceUri, int timeoutMs);
  rapidjson::Document getPrompt(const std::string &promptName,
                               const rapidjson::Document &arguments,
                               int timeoutMs);

private:
  void shutdown(int timeoutMs = 2000);
  void validateInitializeResult(const rapidjson::Document &response) const;

  std::unique_ptr<shared::IHostProcess> process_;
  shared::ToolContext &ctx_;
  std::unique_ptr<IMcpSession> session_;
  int nextId_ = 1;
  bool initialized_ = false;
  bool shutdownSent_ = false;
};

} // namespace firmius::core::mcp

#endif