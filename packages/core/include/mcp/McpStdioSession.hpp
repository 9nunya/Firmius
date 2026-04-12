#ifndef FIRMIUS_CORE_MCP_STDIO_SESSION_HPP
#define FIRMIUS_CORE_MCP_STDIO_SESSION_HPP

#include "ITool.hpp"
#include "mcp/IMcpSession.hpp"

#include <rapidjson/document.h>

#include <string>

namespace firmius::core::mcp {

class McpStdioSession : public IMcpSession {
public:
  McpStdioSession(shared::IHostProcess &process, shared::ToolContext &ctx);

  rapidjson::Document sendRequest(int id, const std::string &method,
                                  const rapidjson::Value &params,
                                  int timeoutMs,
                                  const std::string &stage) override;
  void sendNotification(const std::string &method,
                        const rapidjson::Value &params) override;

private:
  rapidjson::Document readResponseForId(int expectedId, int timeoutMs,
                                        const std::string &stage);
  rapidjson::Document readNextFramedJson(int timeoutMs,
                                         const std::string &stage);

  shared::IHostProcess &process_;
  shared::ToolContext &ctx_;
  std::size_t stdoutOffset_ = 0;
};

} // namespace firmius::core::mcp

#endif