#include "mcp/McpClient.hpp"

#include <rapidjson/document.h>

#include "mcp/McpStdioSession.hpp"

#include <chrono>
#include <stdexcept>

namespace firmius::core::mcp {
namespace {

void ensureJsonRpcSuccessOrThrow(const rapidjson::Document &response,
                                 const std::string &stage) {
  if (!response.IsObject()) {
    throw std::runtime_error("MCP " + stage + " response is not a JSON object");
  }
  if (!response.HasMember("jsonrpc") || !response["jsonrpc"].IsString() ||
      std::string(response["jsonrpc"].GetString()) != "2.0") {
    throw std::runtime_error("MCP " + stage +
                             " response missing jsonrpc=2.0");
  }

  if (response.HasMember("error")) {
    std::string message = "MCP " + stage + " returned error";
    const auto &error = response["error"];
    if (error.IsObject()) {
      if (error.HasMember("message") && error["message"].IsString()) {
        message += ": ";
        message += error["message"].GetString();
      }
      if (error.HasMember("code") && error["code"].IsInt()) {
        message += " (code=" + std::to_string(error["code"].GetInt()) + ")";
      }
    }
    throw std::runtime_error(message);
  }

  if (!response.HasMember("result")) {
    throw std::runtime_error("MCP " + stage + " response missing result");
  }
}

} // namespace

McpClient::McpClient(std::unique_ptr<shared::IHostProcess> process)
    : process_(std::move(process)) {
  if (!process_) {
    throw std::runtime_error("MCP process handle is null");
  }
  session_ = std::make_unique<McpStdioSession>(*process_);
}

McpClient::McpClient(const McpHttpTransportConfig &httpConfig) {
  session_ = std::make_unique<McpHttpSession>(httpConfig);
}

McpClient::McpClient(std::unique_ptr<IMcpSession> session)
    : session_(std::move(session)) {
  if (!session_) {
    throw std::runtime_error("MCP session handle is null");
  }
}

McpClient::~McpClient() {
  try {
    shutdown();
  } catch (...) {
  }

  try {
    if (process_ && process_->inspect().running) {
      process_->kill();
    }
  } catch (...) {
  }
}

void McpClient::initialize(const int timeoutMs, shared::ToolContext *ctx) {
  std::lock_guard<std::mutex> lock(requestMutex_);
  if (initialized_) {
    return;
  }

  rapidjson::Document initParams;
  initParams.SetObject();
  auto &a = initParams.GetAllocator();
  initParams.AddMember("protocolVersion", "2024-11-05", a);
  rapidjson::Value capabilities(rapidjson::kObjectType);
  capabilities.AddMember("tools", rapidjson::Value(rapidjson::kObjectType), a);
  initParams.AddMember("capabilities", capabilities, a);
  rapidjson::Value clientInfo(rapidjson::kObjectType);
  clientInfo.AddMember("name", "firmius", a);
  clientInfo.AddMember("version", "1.0", a);
  initParams.AddMember("clientInfo", clientInfo, a);

  rapidjson::Document initResponse =
      session_->sendRequest(nextId_++, "initialize", initParams, timeoutMs,
                            "initialize", ctx);
  ensureJsonRpcSuccessOrThrow(initResponse, "initialize");
  validateInitializeResult(initResponse);

  rapidjson::Document initializedParams;
  initializedParams.SetObject();
  session_->sendNotification("notifications/initialized", initializedParams);

  initialized_ = true;
}

rapidjson::Document McpClient::listTools(const int timeoutMs, shared::ToolContext *ctx) {
  std::lock_guard<std::mutex> lock(requestMutex_);
  rapidjson::Document listParams;
  listParams.SetObject();

  rapidjson::Document listResponse =
      session_->sendRequest(nextId_++, "tools/list", listParams, timeoutMs,
                            "tools/list", ctx);
  ensureJsonRpcSuccessOrThrow(listResponse, "tools/list");
  return listResponse;
}

rapidjson::Document McpClient::listResources(const int timeoutMs, shared::ToolContext *ctx) {
  std::lock_guard<std::mutex> lock(requestMutex_);
  rapidjson::Document params;
  params.SetObject();

  rapidjson::Document response =
      session_->sendRequest(nextId_++, "resources/list", params, timeoutMs,
                            "resources/list", ctx);
  ensureJsonRpcSuccessOrThrow(response, "resources/list");
  return response;
}

rapidjson::Document McpClient::listPrompts(const int timeoutMs, shared::ToolContext *ctx) {
  std::lock_guard<std::mutex> lock(requestMutex_);
  rapidjson::Document params;
  params.SetObject();

  rapidjson::Document response =
      session_->sendRequest(nextId_++, "prompts/list", params, timeoutMs,
                            "prompts/list", ctx);
  ensureJsonRpcSuccessOrThrow(response, "prompts/list");
  return response;
}

rapidjson::Document McpClient::readResource(const std::string &resourceUri,
                                             const int timeoutMs, shared::ToolContext *ctx) {
  std::lock_guard<std::mutex> lock(requestMutex_);
  rapidjson::Document params;
  params.SetObject();
  auto &a = params.GetAllocator();
  params.AddMember("uri", rapidjson::Value(resourceUri.c_str(), a).Move(), a);

  rapidjson::Document response =
      session_->sendRequest(nextId_++, "resources/read", params, timeoutMs,
                            "resources/read", ctx);
  ensureJsonRpcSuccessOrThrow(response, "resources/read");
  return response;
}

rapidjson::Document McpClient::getPrompt(const std::string &promptName,
                                          const rapidjson::Document &arguments,
                                          const int timeoutMs, shared::ToolContext *ctx) {
  std::lock_guard<std::mutex> lock(requestMutex_);
  rapidjson::Document params;
  params.SetObject();
  auto &a = params.GetAllocator();
  params.AddMember("name", rapidjson::Value(promptName.c_str(), a).Move(), a);
  rapidjson::Value copiedArgs;
  copiedArgs.CopyFrom(arguments, a);
  params.AddMember("arguments", copiedArgs, a);

  rapidjson::Document response =
      session_->sendRequest(nextId_++, "prompts/get", params, timeoutMs,
                            "prompts/get", ctx);
  ensureJsonRpcSuccessOrThrow(response, "prompts/get");
  return response;
}

rapidjson::Document McpClient::callTool(const std::string &toolName,
                                        const rapidjson::Document &arguments,
                                        const int timeoutMs, shared::ToolContext *ctx) {
  std::lock_guard<std::mutex> lock(requestMutex_);
  rapidjson::Document callParams;
  callParams.SetObject();
  auto &a = callParams.GetAllocator();
  callParams.AddMember("name", rapidjson::Value(toolName.c_str(), a).Move(), a);
  rapidjson::Value copiedArgs;
  copiedArgs.CopyFrom(arguments, a);
  callParams.AddMember("arguments", copiedArgs, a);

  rapidjson::Document callResponse =
      session_->sendRequest(nextId_++, "tools/call", callParams, timeoutMs,
                            "tools/call", ctx);
  ensureJsonRpcSuccessOrThrow(callResponse, "tools/call");
  return callResponse;
}

void McpClient::shutdown(const int timeoutMs) {
  (void)timeoutMs;
  std::lock_guard<std::mutex> lock(requestMutex_);
  if (shutdownSent_) {
    return;
  }
  shutdownSent_ = true;

  initialized_ = false;

  if (!process_) {
    return;
  }

  try {
    if (process_ && process_->inspect().running) {
      process_->kill();
    }
  } catch (...) {
  }
}

void McpClient::validateInitializeResult(
    const rapidjson::Document &response) const {
  const auto &result = response["result"];
  if (!result.IsObject()) {
    throw std::runtime_error("MCP initialize result must be an object");
  }
  if (!result.HasMember("capabilities") || !result["capabilities"].IsObject()) {
    throw std::runtime_error(
        "MCP initialize result missing capabilities object");
  }

  const auto &caps = result["capabilities"];
  if (!caps.HasMember("tools") || !caps["tools"].IsObject()) {
    throw std::runtime_error(
        "MCP initialize result missing capabilities.tools object");
  }
}

} // namespace firmius::core::mcp
