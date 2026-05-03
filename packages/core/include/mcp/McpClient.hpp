#ifndef FIRMIUS_CORE_MCP_CLIENT_HPP
#define FIRMIUS_CORE_MCP_CLIENT_HPP

#include "ITool.hpp"
#include "mcp/IMcpSession.hpp"
#include "mcp/McpHttpSession.hpp"

#include <rapidjson/document.h>

#include <memory>
#include <mutex>
#include <string>

namespace firmius::core::mcp {

class McpClient {
public:
  McpClient(std::unique_ptr<shared::IHostProcess> process);
  McpClient(const McpHttpTransportConfig &httpConfig);
  McpClient(std::unique_ptr<IMcpSession> session);
  ~McpClient();

  McpClient(const McpClient &) = delete;
  McpClient &operator=(const McpClient &) = delete;

  void initialize(int timeoutMs, shared::ToolContext *ctx = nullptr);
  rapidjson::Document listTools(int timeoutMs, shared::ToolContext *ctx = nullptr);
  rapidjson::Document callTool(const std::string &toolName,
                               const rapidjson::Document &arguments,
                               int timeoutMs, shared::ToolContext *ctx = nullptr);
  rapidjson::Document listResources(int timeoutMs, shared::ToolContext *ctx = nullptr);
  rapidjson::Document listPrompts(int timeoutMs, shared::ToolContext *ctx = nullptr);
  rapidjson::Document readResource(const std::string &resourceUri, int timeoutMs, shared::ToolContext *ctx = nullptr);
  rapidjson::Document getPrompt(const std::string &promptName,
                               const rapidjson::Document &arguments,
                               int timeoutMs, shared::ToolContext *ctx = nullptr);

  void shutdown(int timeoutMs = 2000);
  bool isInitialized() const { return initialized_; }

private:
  void validateInitializeResult(const rapidjson::Document &response) const;

  std::unique_ptr<shared::IHostProcess> process_;
  std::unique_ptr<IMcpSession> session_;
  mutable std::mutex requestMutex_;
  int nextId_ = 1;
  bool initialized_ = false;
  bool shutdownSent_ = false;
};

} // namespace firmius::core::mcp

#endif
