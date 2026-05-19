#include "mcp/McpStdioSession.hpp"

#include "utils/JsonUtil.hpp"
#include "utils/StringUtil.hpp"

#include <chrono>
#include <rapidjson/error/en.h>
#include <iostream>
#include <stdexcept>

namespace firmius::core::mcp {
namespace {

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

int extractIntId(const rapidjson::Value &id) {
  if (id.IsInt()) {
    return id.GetInt();
  }
  if (id.IsString()) {
    try {
      return std::stoi(id.GetString());
    } catch (...) {
      return -1;
    }
  }
  return -1;
}

} // namespace

McpStdioSession::McpStdioSession(shared::IHostProcess &process)
    : process_(process) {}

rapidjson::Document McpStdioSession::sendRequest(const int id,
                                                 const std::string &method,
                                                 const rapidjson::Value &params,
                                                 const int timeoutMs,
                                                 const std::string &stage,
                                                 shared::ToolContext *ctx) {
  rapidjson::Document request;
  request.SetObject();
  auto &a = request.GetAllocator();
  request.AddMember("jsonrpc", "2.0", a);
  request.AddMember("id", id, a);
  request.AddMember("method", rapidjson::Value(method.c_str(), a).Move(), a);

  rapidjson::Value copiedParams;
  copiedParams.CopyFrom(params, a);
  request.AddMember("params", copiedParams, a);

  const std::string body = firmius::shared::toJsonString(request);
  // Standard MCP stdio transport uses newline-delimited JSON ONLY.
  // Headers like Content-Length are for HTTP/LSP and confuse standard MCP servers.
  const std::string framed = body + "\n";
  process_.write(framed);

  return readResponseForId(id, timeoutMs, stage, ctx);
}

void McpStdioSession::sendNotification(const std::string &method,
                                       const rapidjson::Value &params) {
  rapidjson::Document request;
  request.SetObject();
  auto &a = request.GetAllocator();
  request.AddMember("jsonrpc", "2.0", a);
  request.AddMember("method", rapidjson::Value(method.c_str(), a).Move(), a);

  rapidjson::Value copiedParams;
  copiedParams.CopyFrom(params, a);
  request.AddMember("params", copiedParams, a);

  const std::string body = firmius::shared::toJsonString(request);
  // Standard MCP stdio transport uses newline-delimited JSON ONLY.
  const std::string framed = body + "\n";
  process_.write(framed);
}

rapidjson::Document McpStdioSession::readResponseForId(const int expectedId,
                                                       const int timeoutMs,
                                                       const std::string &stage,
                                                       shared::ToolContext *ctx) {
  while (true) {
    rapidjson::Document message = readNextFramedJson(timeoutMs, stage, ctx);

    if (!message.IsObject()) {
      throw std::runtime_error("MCP " + stage + " response is not a JSON object");
    }

    if (!message.HasMember("id")) {
      continue;
    }

    const int responseId = extractIntId(message["id"]);
    if (responseId != expectedId) {
      continue;
    }

    if (!message.HasMember("jsonrpc") || !message["jsonrpc"].IsString() ||
        std::string(message["jsonrpc"].GetString()) != "2.0") {
      throw std::runtime_error("MCP " + stage +
                               " response missing jsonrpc=2.0");
    }

    return message;
  }
}

rapidjson::Document McpStdioSession::readNextFramedJson(const int timeoutMs,
                                                        const std::string &stage,
                                                        shared::ToolContext *ctx) {
  const auto start = std::chrono::steady_clock::now();

  while (true) {
    if (ctx && ctx->cancelRequested()) {
      process_.kill();
      throw std::runtime_error("Interrupted");
    }

    const auto snapshot = process_.inspect();
    const std::string &stdoutData = snapshot.stdoutData;

    while (stdoutOffset_ < stdoutData.size()) {
      // Try to find Content-Length framing first
      const std::size_t headerPos = stdoutData.find("Content-Length:", stdoutOffset_);
      if (headerPos != std::string::npos) {
        const std::size_t lineEnd = stdoutData.find("\r\n", headerPos);
        const std::size_t bodyMarker = stdoutData.find("\r\n\r\n", headerPos);
        
        if (lineEnd != std::string::npos && bodyMarker != std::string::npos) {
          const std::string headerLine = stdoutData.substr(headerPos, lineEnd - headerPos);
          const std::size_t colonPos = headerLine.find(':');
          
          if (colonPos != std::string::npos) {
            try {
              const std::string lenText = shared::StringUtil::trim(headerLine.substr(colonPos + 1));
              const std::size_t contentLength = static_cast<std::size_t>(std::stoul(lenText));
              const std::size_t payloadStart = bodyMarker + 4;
              
              if (stdoutData.size() >= payloadStart + contentLength) {
                const std::string payload = stdoutData.substr(payloadStart, contentLength);
                stdoutOffset_ = payloadStart + contentLength;
                return parseJsonOrThrow(payload, "MCP " + stage + " response");
              }
              // Need more data for framed payload
              break;
            } catch (...) {
              // Fall back to line-based if Content-Length is invalid
            }
          }
        }
      }

      // Try line-delimited JSON
      const std::size_t nextNewline = stdoutData.find('\n', stdoutOffset_);
      if (nextNewline != std::string::npos) {
        std::string line = stdoutData.substr(stdoutOffset_, nextNewline - stdoutOffset_);
        stdoutOffset_ = nextNewline + 1;
        line = shared::StringUtil::trim(line);
        
        if (!line.empty()) {
          if (line.find("Content-Length:") == 0) {
              // Skip Content-Length headers in line-based mode
              continue;
          }
          if (line[0] == '{') {
            try {
              return parseJsonOrThrow(line, "MCP " + stage + " response");
            } catch (...) {
              // Not valid JSON, might be a log message or partial framed header, keep looking
            }
          }
        }
        continue;
      }

      // No more complete messages in current buffer
      break;
    }

    if (!snapshot.running) {
      throw std::runtime_error("MCP process exited before completing " + stage +
                               " response; stderr: " + snapshot.stderrData);
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    if (elapsed >= timeoutMs) {
      process_.kill();
      throw std::runtime_error("Timeout waiting for MCP " + stage +
                               " response after " + std::to_string(timeoutMs) +
                               "ms");
    }

    if (ctx && !ctx->waitFor(std::chrono::milliseconds(10))) {
      process_.kill();
      throw std::runtime_error("Interrupted");
    }
  }
}

} // namespace firmius::core::mcp
