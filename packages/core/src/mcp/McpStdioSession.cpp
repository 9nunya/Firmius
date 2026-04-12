#include "mcp/McpStdioSession.hpp"

#include "utils/StringUtil.hpp"

#include <chrono>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
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

McpStdioSession::McpStdioSession(shared::IHostProcess &process,
                                 shared::ToolContext &ctx)
    : process_(process), ctx_(ctx) {}

rapidjson::Document McpStdioSession::sendRequest(const int id,
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

  const std::string body = jsonToString(request);
  const std::string framed =
      "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  process_.write(framed);

  return readResponseForId(id, timeoutMs, stage);
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

  const std::string body = jsonToString(request);
  const std::string framed =
      "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  process_.write(framed);
}

rapidjson::Document McpStdioSession::readResponseForId(const int expectedId,
                                                       const int timeoutMs,
                                                       const std::string &stage) {
  while (true) {
    rapidjson::Document message = readNextFramedJson(timeoutMs, stage);

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
                                                        const std::string &stage) {
  const auto start = std::chrono::steady_clock::now();

  while (true) {
    if (ctx_.cancelRequested()) {
      process_.kill();
      throw std::runtime_error("Interrupted");
    }

    const auto snapshot = process_.inspect();
    const std::string &stdoutData = snapshot.stdoutData;

    if (stdoutOffset_ < stdoutData.size()) {
      const std::size_t headerPos = stdoutData.find("Content-Length:", stdoutOffset_);
      if (headerPos == std::string::npos) {
        throw std::runtime_error("MCP " + stage +
                                 " response missing Content-Length header");
      }

      const std::size_t lineEnd = stdoutData.find("\r\n", headerPos);
      if (lineEnd != std::string::npos) {
        const std::string headerLine = stdoutData.substr(headerPos, lineEnd - headerPos);
        const std::size_t colonPos = headerLine.find(':');
        if (colonPos == std::string::npos) {
          throw std::runtime_error("MCP " + stage +
                                   " response Content-Length malformed");
        }

        std::size_t contentLength = 0;
        try {
          const std::string lenText =
              shared::StringUtil::trim(headerLine.substr(colonPos + 1));
          contentLength = static_cast<std::size_t>(std::stoul(lenText));
        } catch (...) {
          throw std::runtime_error("MCP " + stage +
                                   " response Content-Length is invalid");
        }

        const std::size_t bodyMarker = stdoutData.find("\r\n\r\n", headerPos);
        if (bodyMarker != std::string::npos) {
          const std::size_t payloadStart = bodyMarker + 4;
          if (stdoutData.size() >= payloadStart + contentLength) {
            const std::string payload =
                stdoutData.substr(payloadStart, contentLength);
            stdoutOffset_ = payloadStart + contentLength;
            return parseJsonOrThrow(payload, "MCP " + stage + " response");
          }
        }
      }
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

    if (!ctx_.waitFor(std::chrono::milliseconds(10))) {
      process_.kill();
      throw std::runtime_error("Interrupted");
    }
  }
}

} // namespace firmius::core::mcp
