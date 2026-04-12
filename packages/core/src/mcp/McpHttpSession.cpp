#include "mcp/McpHttpSession.hpp"

#include "utils/GCPHttpClient.hpp"

#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <stdexcept>

namespace firmius::core::mcp {
namespace {

std::string jsonToString(const rapidjson::Value &value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return std::string(buffer.GetString());
}

rapidjson::Document parseJsonOrThrow(const std::string &jsonText,
                                     const std::string &context) {
  rapidjson::Document doc;
  doc.Parse(jsonText.c_str());
  if (doc.HasParseError()) {
    throw std::runtime_error(context + ": invalid JSON: " +
                             rapidjson::GetParseError_En(doc.GetParseError()));
  }
  return doc;
}

} // namespace

McpHttpSession::McpHttpSession(const McpHttpTransportConfig &config,
                               shared::ToolContext &ctx)
    : McpHttpSession(config, ctx, &McpHttpSession::defaultSend) {}

McpHttpSession::McpHttpSession(const McpHttpTransportConfig &config,
                               shared::ToolContext &ctx, McpHttpSender sender)
    : config_(config), ctx_(ctx), sender_(std::move(sender)) {
  if (!sender_) {
    throw std::runtime_error("MCP HTTP sender is not configured");
  }
}

rapidjson::Document McpHttpSession::sendRequest(const int id,
                                                const std::string &method,
                                                const rapidjson::Value &params,
                                                const int timeoutMs,
                                                const std::string &stage) {
  rapidjson::Document request;
  request.SetObject();
  auto &a = request.GetAllocator();
  request.AddMember("jsonrpc", "2.0", a);
  request.AddMember("id", id, a);
  request.AddMember("method", rapidjson::Value(method.c_str(), a).Move(), a);

  rapidjson::Value copiedParams;
  copiedParams.CopyFrom(params, a);
  request.AddMember("params", copiedParams, a);

  return sendJsonRpc(request, timeoutMs, stage);
}

void McpHttpSession::sendNotification(const std::string &method,
                                      const rapidjson::Value &params) {
  rapidjson::Document request;
  request.SetObject();
  auto &a = request.GetAllocator();
  request.AddMember("jsonrpc", "2.0", a);
  request.AddMember("method", rapidjson::Value(method.c_str(), a).Move(), a);

  rapidjson::Value copiedParams;
  copiedParams.CopyFrom(params, a);
  request.AddMember("params", copiedParams, a);

  (void)sendJsonRpc(request, 2000, method);
}

rapidjson::Document McpHttpSession::sendJsonRpc(const rapidjson::Document &request,
                                                const int timeoutMs,
                                                const std::string &stage) const {
  if (ctx_.cancelRequested()) {
    throw std::runtime_error("Interrupted");
  }

  const std::string requestBody = jsonToString(request);
  const McpHttpResponse response = sender_(config_, requestBody, timeoutMs);

  if (response.error.size() > 0) {
    throw std::runtime_error("MCP " + stage + " HTTP request failed: " +
                             response.error);
  }
  if (response.code < 200 || response.code >= 300) {
    std::string message = "MCP " + stage + " HTTP " +
                          std::to_string(response.code);
    if (!response.body.empty()) {
      message += ": " + response.body;
    }
    throw std::runtime_error(message);
  }
  if (response.body.empty()) {
    throw std::runtime_error("MCP " + stage + " HTTP response body is empty");
  }

  if (ctx_.cancelRequested()) {
    throw std::runtime_error("Interrupted");
  }

  return parseJsonOrThrow(response.body, "MCP " + stage + " response");
}

McpHttpResponse McpHttpSession::defaultSend(const McpHttpTransportConfig &config,
                                            const std::string &requestBody,
                                            const int timeoutMs) {
  if (config.url.empty()) {
    return {0, "", "MCP HTTP URL is empty", {}};
  }
  if (config.allowInsecureTls) {
    return {0, "",
            "MCP HTTP allowInsecureTls is not supported by current HTTP client",
            {}};
  }
  if (!config.caCertPath.empty()) {
    return {0, "",
            "MCP HTTP caCertPath is not supported by current HTTP client", {}};
  }

  utils::GCPHttpClient http("firmius-mcp/1.0");
  http.setContentType("application/json");

  if (!config.authBearerToken.empty()) {
    const std::string headerName =
        config.authHeader.empty() ? "Authorization" : config.authHeader;
    http.addHeader(headerName, "Bearer " + config.authBearerToken);
  }

  const int timeoutSeconds = std::max(1, (timeoutMs + 999) / 1000);
  const auto response = http.post(config.url, requestBody, timeoutSeconds);
  return {response.code, response.body, response.error, response.headers};
}

} // namespace firmius::core::mcp