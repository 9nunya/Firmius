#ifndef FIRMIUS_CORE_MCP_IMCP_SESSION_HPP
#define FIRMIUS_CORE_MCP_IMCP_SESSION_HPP

#include "ITool.hpp"
#include <rapidjson/document.h>

#include <string>

namespace firmius::core::mcp {

class IMcpSession {
public:
  virtual ~IMcpSession() = default;

  virtual rapidjson::Document sendRequest(int id, const std::string &method,
                                          const rapidjson::Value &params,
                                          int timeoutMs,
                                          const std::string &stage,
                                          shared::ToolContext *ctx = nullptr) = 0;
  virtual void sendNotification(const std::string &method,
                                const rapidjson::Value &params) = 0;
};

} // namespace firmius::core::mcp

#endif