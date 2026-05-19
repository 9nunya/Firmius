#ifndef FIRMIUS_CORE_MCPHTTPSESSION_HPP
#define FIRMIUS_CORE_MCPHTTPSESSION_HPP

#include "ITool.hpp"
#include "mcp/IMcpSession.hpp"

#include <rapidjson/fwd.h>

#include <functional>
#include <map>
#include <string>

namespace firmius::core::mcp {

struct McpHttpTransportConfig {
  std::string url;
  std::string authHeader = "Authorization";
  std::string authBearerToken;
  bool allowInsecureTls = false;
  std::string caCertPath;
};

struct McpHttpResponse {
  long code = 0;
  std::string body;
  std::string error;
  std::map<std::string, std::string> headers;
};

using McpHttpSender =
    std::function<McpHttpResponse(const McpHttpTransportConfig &config,
                                  const std::string &requestBody,
                                  int timeoutMs)>;

class McpHttpSession : public IMcpSession {
public:
  McpHttpSession(const McpHttpTransportConfig &config);
  McpHttpSession(const McpHttpTransportConfig &config,
                 McpHttpSender sender);

  rapidjson::Document sendRequest(int id, const std::string &method,
                                  const rapidjson::Value &params,
                                  int timeoutMs,
                                  const std::string &stage,
                                  shared::ToolContext *ctx = nullptr) override;
  void sendNotification(const std::string &method,
                        const rapidjson::Value &params) override;

private:
  rapidjson::Document sendJsonRpc(const rapidjson::Document &request,
                                  int timeoutMs,
                                  const std::string &stage,
                                  shared::ToolContext *ctx = nullptr) const;
  static McpHttpResponse defaultSend(const McpHttpTransportConfig &config,
                                     const std::string &requestBody,
                                     int timeoutMs);

  McpHttpTransportConfig config_;
  McpHttpSender sender_;
};

} // namespace firmius::core::mcp

#endif