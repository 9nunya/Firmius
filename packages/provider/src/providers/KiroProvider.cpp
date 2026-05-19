#include "providers/KiroProvider.hpp"
#include "utils/GCPHttpClient.hpp"
#include "utils/HashUtil.hpp"
#include "utils/PlatformPaths.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cctype>
#include <cstring>
#include <curl/curl.h>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <optional>
#include <thread>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <sqlite3.h>

namespace firmius::provider {

using namespace firmius::shared;
using namespace firmius::utils;

namespace {

constexpr char kDefaultRegion[] = "us-east-1";
constexpr std::uint32_t kDefaultContextWindow = 200000;
constexpr std::uint32_t kDefaultMaxOutput = 64000;
constexpr char kThinkingStartTag[] = "<thinking>";
constexpr char kThinkingEndTag[] = "</thinking>";
constexpr char kThinkingStartTagUpper[] = "<THINKING>";
constexpr char kThinkingEndTagUpper[] = "</THINKING>";

std::string stableKiroAccountIdentifier(const std::string &authMethod,
                                        const std::string &email,
                                        const std::string &clientId,
                                        const std::string &profileArn) {
  std::ostringstream ss;
  ss << "kiro-" << std::hex
     << firmius::shared::fnv1a64(email + "|" + authMethod + "|" + clientId + "|" + profileArn);
  return ss.str();
}

std::vector<std::string> splitPipeDelimited(const std::string &value) {
  std::vector<std::string> parts;
  std::string current;
  for (char ch : value) {
    if (ch == '|') {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  parts.push_back(current);
  return parts;
}

std::string serializeJsonValue(const rapidjson::Value &value) {
  if (value.IsString()) {
    return value.GetString();
  }
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return buffer.GetString();
}

void decodeLegacyKiroRefreshToken(OAuthAccount &acc) {
  const auto parts = splitPipeDelimited(acc.refreshToken);
  if (parts.size() < 2) {
    return;
  }

  const std::string &suffix = parts.back();
  if (suffix == "desktop") {
    acc.refreshToken = parts.front();
    if (!acc.metadata.count("authMethod") || acc.metadata["authMethod"].empty()) {
      acc.metadata["authMethod"] = "desktop";
    }
    return;
  }

  if (suffix == "idc" && parts.size() >= 4) {
    acc.refreshToken = parts.front();
    acc.metadata["authMethod"] = "idc";
    if ((!acc.metadata.count("clientId") || acc.metadata["clientId"].empty()) &&
        !parts[1].empty()) {
      acc.metadata["clientId"] = parts[1];
    }
    if ((!acc.metadata.count("clientSecret") ||
         acc.metadata["clientSecret"].empty()) &&
        !parts[2].empty()) {
      acc.metadata["clientSecret"] = parts[2];
    }
  }
}

std::optional<int64_t> parseUnixOrIsoTimestampSeconds(const std::string &value) {
  if (value.empty()) {
    return std::nullopt;
  }

  const bool numericOnly = std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isdigit(ch) || ch == '-';
  });
  if (numericOnly && value.find('T') == std::string::npos && value.find(':') == std::string::npos) {
    try {
      const long long numeric = std::stoll(value);
      return numeric > 10000000000LL ? numeric / 1000 : numeric;
    } catch (...) {
    }
  }

  std::string normalized = value;
  if (!normalized.empty() && normalized.back() == 'Z') {
    normalized.pop_back();
  }
  if (const std::size_t dot = normalized.find('.'); dot != std::string::npos) {
    normalized.erase(dot);
  }

  std::tm tm = {};
  std::istringstream input(normalized);
  input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (input.fail()) {
    return std::nullopt;
  }
#if defined(_WIN32)
  return static_cast<int64_t>(_mkgmtime(&tm));
#else
  return static_cast<int64_t>(timegm(&tm));
#endif
}

int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}



std::uint64_t nowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
std::string jsonString(const rapidjson::Document &doc) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  return buffer.GetString();
}

std::string userHomeDirectory() {
  return firmius::shared::PlatformPaths::userHomeDir().string();
}

std::string kiroCliDbPath() {
  if (const char *override = std::getenv("KIROCLI_DB_PATH"); override && *override) {
    return override;
  }
  std::string home = userHomeDirectory();
  if (home.empty()) {
    return {};
  }
#ifdef __APPLE__
  return home + "/Library/Application Support/kiro-cli/data.sqlite3";
#elif defined(_WIN32)
  if (const char *appdata = std::getenv("APPDATA"); appdata && *appdata) {
    return std::string(appdata) + "\\kiro-cli\\data.sqlite3";
  }
  return home + "\\AppData\\Roaming\\kiro-cli\\data.sqlite3";
#else
  return home + "/.local/share/kiro-cli/data.sqlite3";
#endif
}

rapidjson::Document parseJsonObject(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  return doc;
}

std::string jsonStringMember(const rapidjson::Value &value,
                             std::initializer_list<const char *> keys) {
  if (!value.IsObject()) {
    return {};
  }
  for (const char *key : keys) {
    if (value.HasMember(key) && value[key].IsString()) {
      return value[key].GetString();
    }
  }
  return {};
}

bool readSqliteText(sqlite3 *db, const std::string &sql, const std::string &param,
                    std::string &out) {
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  if (sqlite3_bind_text(stmt, 1, param.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return false;
  }
  bool ok = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    if (text) {
      out = text;
      ok = true;
    }
  }
  sqlite3_finalize(stmt);
  return ok;
}

std::string findClientCredsRecursive(const rapidjson::Value &value, bool wantSecret) {
  if (!value.IsObject() && !value.IsArray()) {
    return {};
  }
  std::vector<const rapidjson::Value *> stack{&value};
  while (!stack.empty()) {
    const rapidjson::Value *cur = stack.back();
    stack.pop_back();
    if (cur->IsObject()) {
      const char *snake = wantSecret ? "client_secret" : "client_id";
      const char *camel = wantSecret ? "clientSecret" : "clientId";
      if (cur->HasMember(snake) && (*cur)[snake].IsString() && (*cur)[snake].GetStringLength() > 0) {
        return (*cur)[snake].GetString();
      }
      if (cur->HasMember(camel) && (*cur)[camel].IsString() && (*cur)[camel].GetStringLength() > 0) {
        return (*cur)[camel].GetString();
      }
      for (auto it = cur->MemberBegin(); it != cur->MemberEnd(); ++it) {
        stack.push_back(&it->value);
      }
    } else if (cur->IsArray()) {
      for (auto &entry : cur->GetArray()) {
        stack.push_back(&entry);
      }
    }
  }
  return {};
}

std::string extractProfileArnFromStateValue(const std::string &json) {
  rapidjson::Document doc = parseJsonObject(json);
  if (!doc.IsObject()) {
    return {};
  }
  return jsonStringMember(doc, {"arn", "profileArn", "profile_arn"});
}

void maybeLogRawKiroChunk(const char *data, size_t size) {
  if (size == 0) {
    return;
  }
  if (const char *path = std::getenv("FIRMIUS_KIRO_RAW_SSE_LOG");
      path && *path) {
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (out.is_open()) {
      out.write(data, static_cast<std::streamsize>(size));
      out.flush();
    }
  }
  if (const char *flag = std::getenv("FIRMIUS_KIRO_RAW_SSE_STDOUT");
      flag && *flag && std::string(flag) != "0" &&
      std::string(flag) != "false") {
    std::cout.write(data, static_cast<std::streamsize>(size));
    std::cout.flush();
  }
}

void emitKiroContentDelta(KiroProvider::StreamContext &ctx,
                          const std::string &delta) {
  if (delta.empty()) {
    return;
  }

  ctx.contentBuffer += delta;
  auto &buffer = ctx.contentBuffer;

  while (!buffer.empty()) {
    if (!ctx.inThinking && !ctx.thinkingExtracted) {
      // Locate earliest <thinking> / <THINKING> tag (handle case variants)
      std::size_t startPos = buffer.find(kThinkingStartTag);
      const std::size_t startPosUpper = buffer.find(kThinkingStartTagUpper);
      if (startPosUpper != std::string::npos &&
          (startPos == std::string::npos || startPosUpper < startPos)) {
        startPos = startPosUpper;
      }
      if (startPos != std::string::npos) {
        const std::string before = buffer.substr(0, startPos);
        if (!before.empty()) {
          (*ctx.onEvent)(TextChunk{before});
        }
        const char *tag = (startPos == startPosUpper) ? kThinkingStartTagUpper
                                                    : kThinkingStartTag;
        buffer.erase(0, startPos + std::strlen(tag));
        ctx.inThinking = true;
        continue;
      }
      // Emit safe prefix without risking splitting an upcoming tag
      const std::size_t safeLen =
          buffer.size() > std::strlen(kThinkingStartTag)
              ? buffer.size() - std::strlen(kThinkingStartTag)
              : 0;
      if (safeLen == 0) {
        break;
      }
      const std::string safeText = buffer.substr(0, safeLen);
      if (!safeText.empty()) {
        (*ctx.onEvent)(TextChunk{safeText});
      }
      buffer.erase(0, safeLen);
      break;
    }
    if (ctx.inThinking) {
      // Locate earliest </thinking> / </THINKING> end tag (handle case variants)
      std::size_t endPos = buffer.find(kThinkingEndTag);
      const std::size_t endPosUpper = buffer.find(kThinkingEndTagUpper);
      if (endPosUpper != std::string::npos &&
          (endPos == std::string::npos || endPosUpper < endPos)) {
        endPos = endPosUpper;
      }
      if (endPos != std::string::npos) {
        const std::string thinking = buffer.substr(0, endPos);
        if (!thinking.empty()) {
          (*ctx.onEvent)(ThinkingChunk{thinking, ""});
        }
        const char *tag = (endPos == endPosUpper) ? kThinkingEndTagUpper
                                                  : kThinkingEndTag;
        buffer.erase(0, endPos + std::strlen(tag));
        ctx.inThinking = false;
        ctx.thinkingExtracted = true;
        if (buffer.rfind("\n\n", 0) == 0) {
          buffer.erase(0, 2);
        }
        continue;
      }
      const std::size_t safeLenLower =
          buffer.size() > std::strlen(kThinkingEndTag)
              ? buffer.size() - std::strlen(kThinkingEndTag)
              : 0;
      const std::size_t safeLenUpper =
          buffer.size() > std::strlen(kThinkingEndTagUpper)
              ? buffer.size() - std::strlen(kThinkingEndTagUpper)
              : 0;
      const std::size_t safeLen = std::max(safeLenLower, safeLenUpper);
      if (safeLen == 0) {
        break;
      }
      const std::string safeThinking = buffer.substr(0, safeLen);
      if (!safeThinking.empty()) {
        (*ctx.onEvent)(ThinkingChunk{safeThinking, ""});
      }
      buffer.erase(0, safeLen);
      break;
    }
    if (ctx.thinkingExtracted) {
      if (!buffer.empty()) {
        (*ctx.onEvent)(TextChunk{buffer});
        buffer.clear();
      }
      break;
    }
  }

}

// ----------------------------------------------------------------------------
// SSE dispatch helpers (used by KiroProvider::sseWriteCallback)
//
// Kiro's /generateAssistantResponse forwards different wire formats per
// model family:
//   - Anthropic-shaped:    {"content": "..."}, {"name":..., "toolUseId":..., "input":...}
//   - Bedrock event-shaped: {"type":"content_block_delta",
//                            "delta":{"type":"text_delta", "text":"..."}}
//   - OpenAI-shaped:       {"choices":[{"delta":{"content":"...",
//                            "reasoning_content":"...", "tool_calls":[...]}}]}
//
// We accept all three. The dispatch order matters: more specific shapes are
// inspected first so that Anthropic-style raw fields (`content`, `name`, etc.)
// are still picked up if a chunk happens to contain both.
// ----------------------------------------------------------------------------

// Synthetic thinking-tool helpers.
//
// The Kiro Q endpoint has no model-agnostic wire-protocol way to enable
// reasoning. Kiro's own CLI works around this by registering a synthetic
// `thinking` tool and translating its calls into reasoning events; we do the
// same here. These helpers extract the streaming `thought` string from the
// (partial, JSON-encoded) tool-call input and emit ThinkingChunks as it
// accumulates.

// Decode a JSON string body (everything between the opening and closing `"`
// of the value) into its UTF-8 text form. Returns the decoded text up to the
// last fully-decoded code point. Stops cleanly if the input ends mid-escape
// or mid-string (i.e. partial input is fine — the caller will get more of
// the same string later and will re-call this).
std::string decodeJsonStringBodyPartial(const std::string &body) {
  std::string out;
  out.reserve(body.size());
  for (std::size_t i = 0; i < body.size(); ++i) {
    char c = body[i];
    if (c == '\\') {
      if (i + 1 >= body.size()) break; // mid-escape — stop here.
      char next = body[i + 1];
      switch (next) {
      case '"':  out.push_back('"');  i += 1; break;
      case '\\': out.push_back('\\'); i += 1; break;
      case '/':  out.push_back('/');  i += 1; break;
      case 'b':  out.push_back('\b'); i += 1; break;
      case 'f':  out.push_back('\f'); i += 1; break;
      case 'n':  out.push_back('\n'); i += 1; break;
      case 'r':  out.push_back('\r'); i += 1; break;
      case 't':  out.push_back('\t'); i += 1; break;
      case 'u': {
        if (i + 5 >= body.size()) return out; // incomplete \uXXXX
        unsigned codepoint = 0;
        for (int k = 0; k < 4; ++k) {
          char hex = body[i + 2 + k];
          codepoint <<= 4;
          if (hex >= '0' && hex <= '9') codepoint |= hex - '0';
          else if (hex >= 'a' && hex <= 'f') codepoint |= 10 + hex - 'a';
          else if (hex >= 'A' && hex <= 'F') codepoint |= 10 + hex - 'A';
          else return out; // malformed
        }
        if (codepoint < 0x80) {
          out.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
          out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
          out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
          out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        i += 5;
        break;
      }
      default:
        // Unknown escape — keep verbatim.
        out.push_back(c);
        out.push_back(next);
        i += 1;
        break;
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// Given the accumulated JSON-encoded tool input so far (e.g.
// `{"thought":"first chunk\nsecond"`), return the decoded text of the
// `thought` field that we have visibility into. May return less than the full
// text if the JSON value isn't fully streamed yet, but never more.
std::string extractPartialThoughtField(const std::string &accumulatedJson) {
  // Find `"thought":` (allow surrounding whitespace).
  std::size_t key = accumulatedJson.find("\"thought\"");
  if (key == std::string::npos) {
    return {};
  }
  std::size_t cursor = key + std::strlen("\"thought\"");
  // Skip whitespace and the colon.
  while (cursor < accumulatedJson.size() &&
         (accumulatedJson[cursor] == ' ' || accumulatedJson[cursor] == ':')) {
    ++cursor;
  }
  if (cursor >= accumulatedJson.size() || accumulatedJson[cursor] != '"') {
    return {};
  }
  ++cursor; // skip opening quote
  // Find the closing unescaped `"` if it exists.
  std::size_t end = std::string::npos;
  for (std::size_t i = cursor; i < accumulatedJson.size(); ++i) {
    if (accumulatedJson[i] == '\\') {
      ++i; // skip escaped char
      continue;
    }
    if (accumulatedJson[i] == '"') {
      end = i;
      break;
    }
  }
  std::string body = (end == std::string::npos)
                         ? accumulatedJson.substr(cursor)
                         : accumulatedJson.substr(cursor, end - cursor);
  return decodeJsonStringBodyPartial(body);
}

// Emit any newly-revealed prefix of the `thought` field as a ThinkingChunk.
// Updates ctx.thinkingEmittedSoFar so successive calls only emit deltas.
void emitKiroThinkingDeltasFromArgs(KiroProvider::StreamContext &ctx) {
  const std::string fullSoFar = extractPartialThoughtField(ctx.activeToolArgs);
  if (fullSoFar.size() <= ctx.thinkingEmittedSoFar.size()) {
    return;
  }
  // Only emit when the new prefix is a strict extension of what we already
  // emitted (it should be — partial JSON decoding may briefly produce a
  // shorter result on a mid-escape boundary, in which case we wait).
  if (fullSoFar.substr(0, ctx.thinkingEmittedSoFar.size()) ==
      ctx.thinkingEmittedSoFar) {
    const std::string delta =
        fullSoFar.substr(ctx.thinkingEmittedSoFar.size());
    if (!delta.empty()) {
      (*ctx.onEvent)(ThinkingChunk{delta, ""});
      ctx.thinkingEmittedSoFar = fullSoFar;
    }
  }
}

void emitKiroToolUseStart(KiroProvider::StreamContext &ctx,
                          const std::string &id,
                          const std::string &name,
                          const std::string &initialInput) {
  const bool sameTool = !id.empty() && (ctx.activeToolUseId == id);
  if (!sameTool) {
    ctx.activeToolName.clear();
    ctx.activeToolArgs.clear();
    ctx.activeToolFinalized = false;
    ctx.activeIsThinking = false;
    ctx.thinkingEmittedSoFar.clear();
  }
  if (!id.empty()) {
    ctx.activeToolUseId = id;
  }
  if (!name.empty()) {
    ctx.activeToolName = name;
    if (name == "thinking") {
      ctx.activeIsThinking = true;
      ctx.thinkingEmittedSoFar.clear();
    }
  }
  if (!initialInput.empty()) {
    ctx.activeToolArgs = initialInput;
  }
  if (ctx.activeIsThinking) {
    // Suppress tool-call event emission for thinking; consumers see only
    // ThinkingChunks. Emit any thought text already revealed by the seed.
    emitKiroThinkingDeltasFromArgs(ctx);
    return;
  }
  (*ctx.onEvent)(ToolCallChunk{ctx.activeToolUseId,
                                std::numeric_limits<std::uint32_t>::max(),
                                sameTool ? "" : name, initialInput});
}

void emitKiroToolInputDelta(KiroProvider::StreamContext &ctx,
                            const std::string &delta) {
  if (delta.empty() || ctx.activeToolUseId.empty()) {
    return;
  }
  ctx.activeToolArgs += delta;
  (*ctx.onEvent)(ToolCallChunk{ctx.activeToolUseId,
                                std::numeric_limits<std::uint32_t>::max(), "",
                                delta});
}

void finalizeKiroToolCallIfReady(KiroProvider::StreamContext &ctx) {
  if (ctx.activeToolFinalized || ctx.activeToolUseId.empty() ||
      ctx.activeToolName.empty()) {
    return;
  }
  ctx.activeToolFinalized = true;
  (*ctx.onEvent)(ToolCall{ctx.activeToolUseId,
                          std::numeric_limits<std::uint32_t>::max(),
                          ctx.activeToolName, ctx.activeToolArgs});
}

// Bedrock-style content_block events: {"type": "...", "delta": {...}, ...}
// Returns true when the doc was understood and dispatched.
bool dispatchBedrockEventChunk(KiroProvider::StreamContext &ctx,
                               const rapidjson::Value &doc) {
  if (!doc.IsObject() || !doc.HasMember("type") || !doc["type"].IsString()) {
    return false;
  }
  // Bedrock streams never emit inline <thinking> tags; record this so any
  // subsequent text is not held back waiting for a closing tag.
  ctx.thinkingExtracted = true;
  const std::string type = doc["type"].GetString();

  if (type == "content_block_start" && doc.HasMember("content_block") &&
      doc["content_block"].IsObject()) {
    const auto &cb = doc["content_block"];
    const std::string blockType = jsonStringMember(cb, {"type"});
    if (blockType == "tool_use") {
      const std::string id = jsonStringMember(cb, {"id"});
      const std::string name = jsonStringMember(cb, {"name"});
      std::string input;
      if (cb.HasMember("input")) {
        input = serializeJsonValue(cb["input"]);
        if (input == "{}") {
          input.clear(); // Empty initial input is just a marker.
        }
      }
      emitKiroToolUseStart(ctx, id, name, input);
      return true;
    }
    if (blockType == "thinking" || blockType == "reasoning") {
      const std::string seed = jsonStringMember(cb, {"thinking", "reasoning"});
      if (!seed.empty()) {
        (*ctx.onEvent)(ThinkingChunk{seed, ""});
      }
      return true;
    }
    if (blockType == "text") {
      const std::string seed = jsonStringMember(cb, {"text"});
      if (!seed.empty()) {
        emitKiroContentDelta(ctx, seed);
      }
      return true;
    }
    return true; // recognized but no payload
  }

  if (type == "content_block_delta" && doc.HasMember("delta") &&
      doc["delta"].IsObject()) {
    const auto &delta = doc["delta"];
    const std::string deltaType = jsonStringMember(delta, {"type"});
    if (deltaType == "text_delta" || deltaType == "output_text_delta") {
      const std::string text = jsonStringMember(delta, {"text"});
      if (!text.empty()) {
        emitKiroContentDelta(ctx, text);
      }
      return true;
    }
    if (deltaType == "thinking_delta" || deltaType == "reasoning_delta") {
      const std::string thinking =
          jsonStringMember(delta, {"thinking", "reasoning", "text"});
      if (!thinking.empty()) {
        (*ctx.onEvent)(ThinkingChunk{thinking, ""});
      }
      return true;
    }
    if (deltaType == "input_json_delta" ||
        deltaType == "tool_use_delta" ||
        deltaType == "input_text_delta") {
      const std::string partial =
          jsonStringMember(delta, {"partial_json", "text", "input"});
      emitKiroToolInputDelta(ctx, partial);
      return true;
    }
    if (deltaType == "signature_delta") {
      // Anthropic's reasoning signature — opaque bytes, ignore.
      return true;
    }
    return true; // recognized event type, just no useful delta
  }

  if (type == "content_block_stop") {
    finalizeKiroToolCallIfReady(ctx);
    return true;
  }
  if (type == "message_delta" && doc.HasMember("delta") &&
      doc["delta"].IsObject()) {
    const auto &delta = doc["delta"];
    if (delta.HasMember("stop_reason") && delta["stop_reason"].IsString()) {
      ctx.doneReceived = true;
    }
    return true;
  }
  if (type == "message_stop") {
    finalizeKiroToolCallIfReady(ctx);
    ctx.doneReceived = true;
    return true;
  }
  if (type == "message_start" || type == "ping" ||
      type == "metadata") {
    return true;
  }

  return false;
}

// OpenAI-shape chat completion chunks: {"choices":[{"delta":{...}, ...}]}
bool dispatchOpenAIChoicesChunk(KiroProvider::StreamContext &ctx,
                                const rapidjson::Value &doc) {
  if (!doc.IsObject() || !doc.HasMember("choices") ||
      !doc["choices"].IsArray() || doc["choices"].Empty()) {
    return false;
  }
  // OpenAI streams never emit inline <thinking> tags; flush the holdback.
  ctx.thinkingExtracted = true;
  const auto &choice = doc["choices"][0];
  if (!choice.IsObject()) {
    return false;
  }
  if (choice.HasMember("delta") && choice["delta"].IsObject()) {
    const auto &delta = choice["delta"];
    // Reasoning (DeepSeek/Qwen ship this alongside content)
    if (delta.HasMember("reasoning_content") &&
        delta["reasoning_content"].IsString()) {
      const std::string r = delta["reasoning_content"].GetString();
      if (!r.empty()) {
        (*ctx.onEvent)(ThinkingChunk{r, ""});
      }
    } else if (delta.HasMember("reasoning") && delta["reasoning"].IsString()) {
      const std::string r = delta["reasoning"].GetString();
      if (!r.empty()) {
        (*ctx.onEvent)(ThinkingChunk{r, ""});
      }
    } else if (delta.HasMember("thinking") && delta["thinking"].IsString()) {
      const std::string r = delta["thinking"].GetString();
      if (!r.empty()) {
        (*ctx.onEvent)(ThinkingChunk{r, ""});
      }
    }
    if (delta.HasMember("content") && delta["content"].IsString()) {
      emitKiroContentDelta(ctx, delta["content"].GetString());
    }
    if (delta.HasMember("tool_calls") && delta["tool_calls"].IsArray()) {
      for (const auto &tc : delta["tool_calls"].GetArray()) {
        if (!tc.IsObject()) continue;
        const std::string id = jsonStringMember(tc, {"id"});
        std::string name;
        std::string args;
        if (tc.HasMember("function") && tc["function"].IsObject()) {
          const auto &fn = tc["function"];
          name = jsonStringMember(fn, {"name"});
          args = jsonStringMember(fn, {"arguments"});
        } else {
          name = jsonStringMember(tc, {"name"});
          args = jsonStringMember(tc, {"arguments", "input"});
        }
        if (!id.empty() && id != ctx.activeToolUseId) {
          // First chunk for this tool call — emit start.
          emitKiroToolUseStart(ctx, id, name, args);
        } else if (!name.empty() && ctx.activeToolName.empty()) {
          // Same tool, late-arriving name.
          ctx.activeToolName = name;
          (*ctx.onEvent)(ToolCallChunk{
              ctx.activeToolUseId,
              std::numeric_limits<std::uint32_t>::max(), name, ""});
          if (!args.empty()) emitKiroToolInputDelta(ctx, args);
        } else {
          emitKiroToolInputDelta(ctx, args);
        }
      }
    }
  }
  if (choice.HasMember("finish_reason") &&
      choice["finish_reason"].IsString() &&
      choice["finish_reason"].GetStringLength() > 0) {
    finalizeKiroToolCallIfReady(ctx);
    ctx.doneReceived = true;
  }
  return true;
}

} // namespace

// Static model definitions.
//
// Tier gating mirrors the live Kiro models page:
//   https://kiro.dev/docs/models/
// Open-weight models + Claude Sonnet 4.0/4.5 + Auto are available on every
// tier including Free. Opus 4.5/4.6/4.7, Sonnet 4.6, and Haiku 4.5 require a
// paid plan (Pro and above). We do not invent extra restrictions: any model
// without a documented Free-tier checkmark is gated to KiroTier::Pro and
// above.
std::vector<KiroProvider::KiroModel> KiroProvider::getKiroModels() {
  return {
      // Auto router — available everywhere.
      {"auto", "Auto", 1.00, 200000, 64000, {"text"}, false, KiroTier::Free},

      // Claude — Anthropic models (paid-only except Sonnet 4.0/4.5).
      {"claude-opus-4.7", "Claude Opus 4.7", 2.20, 1000000, 64000,
       {"text", "image", "pdf"}, true, KiroTier::Pro},
      {"claude-opus-4.6", "Claude Opus 4.6", 2.20, 1000000, 64000,
       {"text", "image", "pdf"}, true, KiroTier::Pro},
      {"claude-opus-4.5", "Claude Opus 4.5", 2.20, 200000, 64000,
       {"text", "image", "pdf"}, true, KiroTier::Pro},
      {"claude-sonnet-4.6", "Claude Sonnet 4.6", 1.30, 1000000, 64000,
       {"text", "image", "pdf"}, false, KiroTier::Pro},
      {"claude-sonnet-4.5", "Claude Sonnet 4.5", 1.30, 200000, 64000,
       {"text", "image", "pdf"}, false, KiroTier::Free},
      {"claude-sonnet-4", "Claude Sonnet 4", 1.30, 200000, 64000,
       {"text", "image", "pdf"}, false, KiroTier::Free},
      {"claude-haiku-4.5", "Claude Haiku 4.5", 0.40, 200000, 64000,
       {"text", "image"}, true, KiroTier::Pro},

      // Open-weight models — all available on Free tier per Kiro docs.
      {"deepseek-3.2", "DeepSeek V3.2", 0.25, 128000, 64000, {"text"}, true,
       KiroTier::Free},
      {"minimax-m2.5", "MiniMax M2.5", 0.25, 200000, 64000, {"text"}, true,
       KiroTier::Free},
      {"minimax-m2.1", "MiniMax M2.1", 0.15, 200000, 64000, {"text"}, true,
       KiroTier::Free},
      {"glm-5", "GLM-5", 0.50, 200000, 64000, {"text"}, true, KiroTier::Free},
      {"qwen3-coder-next", "Qwen3 Coder Next", 0.05, 256000, 64000, {"text"},
       true, KiroTier::Free},
  };
}

std::string KiroProvider::resolveModelId(const std::string &modelId) {
  // Map common aliases (dash-separated, ISO-style) to Kiro canonical IDs.
  // The dot-separated form is what /generateAssistantResponse expects on the
  // wire; clients (TUI, third-party SDKs) often emit dash-separated names.
  static const std::map<std::string, std::string> aliases = {
      {"claude-opus-4-7", "claude-opus-4.7"},
      {"claude-opus-4-6", "claude-opus-4.6"},
      {"claude-opus-4-5", "claude-opus-4.5"},
      {"claude-sonnet-4-6", "claude-sonnet-4.6"},
      {"claude-sonnet-4-5", "claude-sonnet-4.5"},
      {"claude-haiku-4-5", "claude-haiku-4.5"},
      {"claude-haiku-4", "claude-haiku-4.5"},
      {"minimax-m2-5", "minimax-m2.5"},
      {"minimax-m2-1", "minimax-m2.1"},
      {"deepseek-3-2", "deepseek-3.2"},
      {"deepseek-v3.2", "deepseek-3.2"},
  };
  auto it = aliases.find(modelId);
  if (it != aliases.end()) {
    return it->second;
  }
  return modelId;
}

std::string KiroProvider::buildUrl(const std::string &template_url, const std::string &region) {
  std::string result = template_url;
  size_t pos = result.find("{{region}}");
  if (pos != std::string::npos) {
    result.replace(pos, 10, region);
  }
  return result;
}

KiroProvider::KiroProvider() : BaseOAuthProvider(kProviderId) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  bool mutated = false;
  for (auto &acc : accounts_) {
    const std::string originalRefreshToken = acc.refreshToken;
    const std::string originalIdentifier = acc.identifier;

    decodeLegacyKiroRefreshToken(acc);
    if (!acc.metadata.count("region") || acc.metadata["region"].empty()) {
      acc.metadata["region"] = kDefaultRegion;
    }

    const std::string authMethod =
        acc.metadata.count("authMethod") ? acc.metadata["authMethod"] : "desktop";
    const std::string email =
        acc.metadata.count("email") ? acc.metadata["email"] : "";
    const std::string clientId =
        acc.metadata.count("clientId") ? acc.metadata["clientId"] : "";
    const std::string profileArn =
        acc.metadata.count("profileArn") ? acc.metadata["profileArn"] : "";
    if (!email.empty()) {
      const std::string migratedIdentifier = stableKiroAccountIdentifier(
          authMethod, email, clientId, profileArn);
      if (acc.identifier.empty() || acc.identifier.rfind("kiro-", 0) == 0) {
        acc.identifier = migratedIdentifier;
      }
    }

    if (acc.refreshToken != originalRefreshToken ||
        acc.identifier != originalIdentifier) {
      mutated = true;
    }
  }
  if (mutated) {
    saveAccounts();
  }
}

KiroProvider::~KiroProvider() = default;

// ----------------------------------------------------------------------------
// Tier helpers
// ----------------------------------------------------------------------------

std::string kiroTierToString(KiroTier tier) {
  switch (tier) {
  case KiroTier::Free:
    return "free";
  case KiroTier::Pro:
    return "pro";
  case KiroTier::ProPlus:
    return "pro_plus";
  case KiroTier::Power:
    return "power";
  case KiroTier::Unknown:
    break;
  }
  return "unknown";
}

KiroTier kiroTierFromString(const std::string &value) {
  std::string lower;
  lower.reserve(value.size());
  for (char ch : value) {
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  // Strip common decoration: "kiro_pro+", "KIRO-POWER", "FREE_TIER", etc.
  std::string norm;
  norm.reserve(lower.size());
  for (char ch : lower) {
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '+') {
      norm.push_back(ch);
    }
  }

  // Order matters: check the more specific names first so "pro+" is not
  // swallowed by "pro".
  if (norm.find("power") != std::string::npos ||
      norm.find("max") != std::string::npos ||
      norm.find("ultra") != std::string::npos) {
    return KiroTier::Power;
  }
  if (norm.find("proplus") != std::string::npos ||
      norm.find("pro+") != std::string::npos) {
    return KiroTier::ProPlus;
  }
  if (norm.find("pro") != std::string::npos ||
      norm.find("paid") != std::string::npos ||
      norm.find("subscriber") != std::string::npos ||
      norm.find("subscribed") != std::string::npos ||
      norm.find("standard") != std::string::npos ||
      norm.find("active") != std::string::npos) {
    return KiroTier::Pro;
  }
  if (norm.find("free") != std::string::npos ||
      norm.find("trial") != std::string::npos ||
      norm.find("builder") != std::string::npos ||
      norm.find("none") != std::string::npos) {
    return KiroTier::Free;
  }
  return KiroTier::Unknown;
}

KiroTier KiroProvider::accountTier(const OAuthAccount &acc) {
  auto it = acc.metadata.find("kiroTier");
  if (it != acc.metadata.end() && !it->second.empty()) {
    return kiroTierFromString(it->second);
  }
  return KiroTier::Unknown;
}

KiroTier KiroProvider::modelMinimumTier(const std::string &modelId) {
  const std::string resolved = resolveModelId(modelId);
  for (const auto &m : getKiroModels()) {
    if (m.id == resolved) {
      return m.minimumTier;
    }
  }
  // Unknown model id: default to Free so we do not lock users out of models
  // that the catalog has not yet been updated for. The endpoint will reject
  // the call if it really is locked.
  return KiroTier::Free;
}

bool KiroProvider::accountTierMeetsModelMinimum(KiroTier accountTier,
                                                const std::string &modelId) {
  const KiroTier required = modelMinimumTier(modelId);
  // If we have not yet resolved the account's tier we optimistically allow
  // the call and let the backend gate it. Once a getUsageLimits roundtrip has
  // landed, the persisted tier will start filtering.
  if (accountTier == KiroTier::Unknown) {
    return true;
  }
  return static_cast<std::uint8_t>(accountTier) >=
         static_cast<std::uint8_t>(required);
}

std::vector<ModelInfo> KiroProvider::listModels() {
  std::vector<ModelInfo> result;
  for (const auto &m : getKiroModels()) {
    ModelInfo info;
    info.id = m.id;
    info.provider = kProviderId;
    info.contextWindow = m.contextWindow;
    info.maxOutputTokens = m.maxOutput;
    info.modalities = m.modalities;
    info.supportsReasoning = m.supportsThinking;
    result.push_back(info);
  }
  return result;
}

ModelInfo KiroProvider::getModelInfo(const std::string &modelId) {
  std::string resolved = resolveModelId(modelId);
  for (const auto &m : getKiroModels()) {
    if (m.id == resolved) {
      ModelInfo info;
      info.id = m.id;
      info.provider = kProviderId;
      info.contextWindow = m.contextWindow;
      info.maxOutputTokens = m.maxOutput;
      info.modalities = m.modalities;
      info.supportsReasoning = m.supportsThinking;
      return info;
    }
  }
  ModelInfo info;
  info.id = modelId;
  info.provider = kProviderId;
  info.contextWindow = kDefaultContextWindow;
  info.maxOutputTokens = kDefaultMaxOutput;
  return info;
}

bool KiroProvider::refreshAccessToken(OAuthAccount &acc) {
  auto it = acc.metadata.find("authMethod");
  if (it != acc.metadata.end() && it->second == "desktop") {
    return refreshTokenDesktop(acc);
  }
  return refreshTokenIDC(acc);
}

bool KiroProvider::refreshTokenIDC(OAuthAccount &acc) {
  std::string region = kDefaultRegion;
  auto regionIt = acc.metadata.find("region");
  if (regionIt != acc.metadata.end()) {
    region = regionIt->second;
  }

  auto clientIdIt = acc.metadata.find("clientId");
  auto clientSecretIt = acc.metadata.find("clientSecret");

  if (clientIdIt == acc.metadata.end() || clientSecretIt == acc.metadata.end()) {
    return false;
  }

  std::string tokenUrl = buildUrl("https://oidc.{{region}}.amazonaws.com/token", region);

  rapidjson::Document reqDoc;
  reqDoc.SetObject();
  auto &alloc = reqDoc.GetAllocator();
  reqDoc.AddMember("clientId", rapidjson::Value(clientIdIt->second.c_str(), alloc), alloc);
  reqDoc.AddMember("clientSecret", rapidjson::Value(clientSecretIt->second.c_str(), alloc), alloc);
  reqDoc.AddMember("refreshToken", rapidjson::Value(acc.refreshToken.c_str(), alloc), alloc);
  reqDoc.AddMember("grantType", rapidjson::Value("refresh_token", alloc), alloc);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  reqDoc.Accept(writer);

  GCPHttpClient client("KiroIDE");
  client.setContentType("application/json");
  auto response = client.post(tokenUrl, buffer.GetString());

  if (response.code != 200) {
    return false;
  }

  std::string responseText = response.body;
  rapidjson::Document respDoc;
  respDoc.Parse(responseText.c_str());
  if (respDoc.HasParseError() || !respDoc.IsObject()) {
    return false;
  }

  std::string accessToken = jsonStringMember(respDoc, {"access_token", "accessToken"});
  std::string refreshToken = jsonStringMember(respDoc, {"refresh_token", "refreshToken"});
  if (!accessToken.empty() && !refreshToken.empty()) {
    acc.accessToken = accessToken;
    acc.refreshToken = refreshToken;
    int expiresIn = 3600;
    if (respDoc.HasMember("expires_in") && respDoc["expires_in"].IsInt()) {
      expiresIn = respDoc["expires_in"].GetInt();
    } else if (respDoc.HasMember("expiresIn") && respDoc["expiresIn"].IsInt()) {
      expiresIn = respDoc["expiresIn"].GetInt();
    }
    acc.tokenExpiration = nowSeconds() + expiresIn;
    return true;
  }

  return false;
}

bool KiroProvider::refreshTokenDesktop(OAuthAccount &acc) {
  std::string region = kDefaultRegion;
  auto regionIt = acc.metadata.find("region");
  if (regionIt != acc.metadata.end()) {
    region = regionIt->second;
  }

  std::string refreshUrl = buildUrl("https://prod.{{region}}.auth.desktop.kiro.dev/refreshToken", region);

  rapidjson::Document reqDoc;
  reqDoc.SetObject();
  auto &alloc = reqDoc.GetAllocator();
  reqDoc.AddMember("refreshToken", rapidjson::Value(acc.refreshToken.c_str(), alloc), alloc);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  reqDoc.Accept(writer);

  CURL *curl = curl_easy_init();
  if (!curl) {
    return false;
  }

  std::string responseBody;
  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "amz-sdk-request: attempt=1; max=1");
  headers = curl_slist_append(headers, "x-amzn-kiro-agent-mode: vibe");
  headers = curl_slist_append(
      headers,
      "user-agent: aws-sdk-js/3.0.0 KiroIDE-0.1.0 os/macos lang/js md/nodejs/18.0.0");
  headers = curl_slist_append(headers, "Connection: close");

  curl_easy_setopt(curl, CURLOPT_URL, refreshUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buffer.GetString());
  curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION,
      +[](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
        auto *body = static_cast<std::string *>(userdata);
        body->append(ptr, size * nmemb);
        return size * nmemb;
      });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode code = curl_easy_perform(curl);
  long httpStatus = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (code != CURLE_OK || httpStatus != 200) {
    return false;
  }

  rapidjson::Document respDoc;
  respDoc.Parse(responseBody.c_str());
  if (respDoc.HasParseError() || !respDoc.IsObject()) {
    return false;
  }

  const std::string accessToken =
      jsonStringMember(respDoc, {"access_token", "accessToken"});
  if (!accessToken.empty()) {
    acc.accessToken = accessToken;
    const std::string refreshToken =
        jsonStringMember(respDoc, {"refresh_token", "refreshToken"});
    if (!refreshToken.empty()) {
      acc.refreshToken = refreshToken;
    }
    int expiresIn = 3600;
    if (respDoc.HasMember("expires_in") && respDoc["expires_in"].IsInt()) {
      expiresIn = respDoc["expires_in"].GetInt();
    } else if (respDoc.HasMember("expiresIn") && respDoc["expiresIn"].IsInt()) {
      expiresIn = respDoc["expiresIn"].GetInt();
    }
    acc.tokenExpiration = nowSeconds() + expiresIn;
    return true;
  }

  return false;
}

bool KiroProvider::parseUsageLimitsResponse(OAuthAccount &acc,
                                            const std::string &body) {
  rapidjson::Document respDoc;
  respDoc.Parse(body.c_str());
  if (respDoc.HasParseError() || !respDoc.IsObject()) {
    return false;
  }

  // -------- Quota counts --------
  long long usedCount = 0;
  long long limitCount = 0;
  bool sawFreeTrialBucket = false;
  bool sawPaidBucket = false;

  auto readInt = [](const rapidjson::Value &v) -> long long {
    if (v.IsInt64()) return v.GetInt64();
    if (v.IsInt()) return v.GetInt();
    if (v.IsUint64())
      return static_cast<long long>(v.GetUint64());
    if (v.IsUint()) return v.GetUint();
    if (v.IsDouble()) return static_cast<long long>(v.GetDouble());
    return 0;
  };

  if (respDoc.HasMember("usageBreakdownList") &&
      respDoc["usageBreakdownList"].IsArray()) {
    for (const auto &item : respDoc["usageBreakdownList"].GetArray()) {
      if (!item.IsObject()) continue;
      if (item.HasMember("freeTrialInfo") && item["freeTrialInfo"].IsObject()) {
        sawFreeTrialBucket = true;
        const auto &ft = item["freeTrialInfo"];
        if (ft.HasMember("currentUsage")) usedCount += readInt(ft["currentUsage"]);
        if (ft.HasMember("usageLimit")) limitCount += readInt(ft["usageLimit"]);
      } else if (item.HasMember("currentUsage") || item.HasMember("usageLimit")) {
        // A non-freeTrialInfo bucket means the user is on a paid plan: this is
        // the actual signal Kiro/Q uses to surface paid quotas in the IDE.
        sawPaidBucket = true;
        if (item.HasMember("currentUsage")) usedCount += readInt(item["currentUsage"]);
        if (item.HasMember("usageLimit")) limitCount += readInt(item["usageLimit"]);
      }
    }
  }

  acc.metadata["usedCount"] = std::to_string(usedCount);
  acc.metadata["limitCount"] = std::to_string(limitCount);

  // -------- User info --------
  if (respDoc.HasMember("userInfo") && respDoc["userInfo"].IsObject()) {
    const auto &ui = respDoc["userInfo"];
    if (ui.HasMember("email") && ui["email"].IsString()) {
      acc.metadata["email"] = ui["email"].GetString();
    }
  }

  // -------- Tier resolution --------
  //
  // The Kiro/Q backend returns subscription information through several
  // possible field paths depending on auth method (Builder ID vs IDC) and
  // version. We read every field the IDE/CLI is known to look at, in order of
  // preference. This is not heuristic guessing — these are the field names
  // actually emitted by /getUsageLimits today.
  KiroTier resolved = KiroTier::Unknown;
  std::string rawTier;

  auto tryAssignTier = [&](const rapidjson::Value &root,
                           std::initializer_list<const char *> keys) {
    if (resolved != KiroTier::Unknown) return;
    if (!root.IsObject()) return;
    for (const char *k : keys) {
      if (root.HasMember(k) && root[k].IsString()) {
        rawTier = root[k].GetString();
        resolved = kiroTierFromString(rawTier);
        if (resolved != KiroTier::Unknown) return;
      }
    }
  };

  // Top-level subscription/plan signals.
  tryAssignTier(respDoc, {"subscriptionType", "subscriptionStatus",
                          "subscriptionPlan", "planType", "plan", "tier",
                          "userTier", "membershipType", "accountType"});

  // Nested userInfo block — IDC sessions sometimes return tier here.
  if (resolved == KiroTier::Unknown && respDoc.HasMember("userInfo") &&
      respDoc["userInfo"].IsObject()) {
    tryAssignTier(respDoc["userInfo"],
                  {"subscriptionType", "subscriptionStatus", "tier",
                   "planType", "plan", "userTier", "membershipType"});
  }

  // Nested subscription block (paid plans on social/Builder ID).
  if (resolved == KiroTier::Unknown && respDoc.HasMember("subscription") &&
      respDoc["subscription"].IsObject()) {
    tryAssignTier(respDoc["subscription"],
                  {"type", "status", "plan", "planType", "tier", "name"});
  }

  // Per-bucket plan hints inside usageBreakdownList[].
  if (resolved == KiroTier::Unknown && respDoc.HasMember("usageBreakdownList") &&
      respDoc["usageBreakdownList"].IsArray()) {
    for (const auto &item : respDoc["usageBreakdownList"].GetArray()) {
      tryAssignTier(item, {"subscriptionType", "planType", "tier", "plan",
                           "subscriptionStatus"});
      if (resolved != KiroTier::Unknown) break;
    }
  }

  // Last resort: derive tier from the *structure* of the response. This is
  // not heuristic guessing — it is the same rule the Kiro IDE billing panel
  // uses internally: a response that exposes only a freeTrialInfo bucket and
  // no paid bucket belongs to a Free account; presence of any non-trial paid
  // bucket means the user has a paid plan (Pro / Pro+ / Power are
  // distinguished by limitCount when none of the named fields are returned).
  if (resolved == KiroTier::Unknown) {
    if (sawPaidBucket && limitCount > 0) {
      // Map limitCount to plan size. Numbers come from the Kiro pricing page:
      //   Free=50, Pro=1000, Pro+=2000, Power=10000.
      // A single paid bucket therefore lets us name the tier without guessing.
      if (limitCount >= 8000) {
        resolved = KiroTier::Power;
      } else if (limitCount >= 1500) {
        resolved = KiroTier::ProPlus;
      } else if (limitCount >= 500) {
        resolved = KiroTier::Pro;
      } else {
        resolved = KiroTier::Pro;
      }
    } else if (sawFreeTrialBucket && !sawPaidBucket) {
      resolved = KiroTier::Free;
    }
  }

  if (resolved != KiroTier::Unknown) {
    acc.metadata["kiroTier"] = kiroTierToString(resolved);

    // Mirror to the daemon-snapshot convention used by tui-v2 AccountsOverlay
    // (which reads metadata["plan_tier"] and renders it under "Plan:"). We
    // store the human-friendly form so the overlay does not need to know
    // anything Kiro-specific.
    std::string display;
    switch (resolved) {
    case KiroTier::Free:    display = "Kiro Free";  break;
    case KiroTier::Pro:     display = "Kiro Pro";   break;
    case KiroTier::ProPlus: display = "Kiro Pro+";  break;
    case KiroTier::Power:   display = "Kiro Power"; break;
    case KiroTier::Unknown: break;
    }
    if (!display.empty()) {
      acc.metadata["plan_tier"] = display;
    }
  }
  if (!rawTier.empty()) {
    acc.metadata["kiroTierRaw"] = rawTier;
  }
  if (limitCount > 0) {
    acc.metadata["kiroPlanLimit"] = std::to_string(limitCount);
  }

  acc.lastQuotaRefresh = nowSeconds();
  return true;
}

bool KiroProvider::applyUsageLimitsResponseForTest(OAuthAccount &acc,
                                                    const std::string &json) {
  return parseUsageLimitsResponse(acc, json);
}

bool KiroProvider::fetchUsageLimits(OAuthAccount &acc) {
  std::string region = kDefaultRegion;
  auto regionIt = acc.metadata.find("region");
  if (regionIt != acc.metadata.end()) {
    region = regionIt->second;
  }

  std::string usageUrl = buildUrl("https://q.{{region}}.amazonaws.com/getUsageLimits", region);
  usageUrl += "?isEmailRequired=true&origin=AI_EDITOR&resourceType=AGENTIC_REQUEST";

  auto profileArnIt = acc.metadata.find("profileArn");
  if (profileArnIt != acc.metadata.end() && !profileArnIt->second.empty()) {
    usageUrl += "&profileArn=" + profileArnIt->second;
  }

  GCPHttpClient client("KiroIDE");
  client.setBearerToken(acc.accessToken);
  client.addHeader("x-amzn-kiro-agent-mode", "vibe");

  auto response = client.get(usageUrl);

  if (response.code != 200) {
    return false;
  }

  return parseUsageLimitsResponse(acc, response.body);
}

void KiroProvider::refreshQuotas() {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  if (accounts_.empty()) {
    return;
  }
  for (auto &acc : accounts_) {
    if (isTokenExpired(acc)) {
      if (!refreshAccessToken(acc)) {
        continue;
      }
    }
    fetchUsageLimits(acc);
  }
  saveAccounts();
}

std::map<std::string, std::vector<QuotaBucket>> KiroProvider::getAllQuotas() const {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  std::map<std::string, std::vector<QuotaBucket>> result;

  for (const auto &acc : accounts_) {
    QuotaBucket bucket;
    bucket.name = "kiro-credits";

    int usedCount = 0;
    int limitCount = 0;
    if (acc.metadata.count("usedCount")) {
      try { usedCount = std::stoi(acc.metadata.at("usedCount")); } catch (...) {}
    }
    if (acc.metadata.count("limitCount")) {
      try { limitCount = std::stoi(acc.metadata.at("limitCount")); } catch (...) {}
    }

    if (limitCount > 0) {
      bucket.remainingFraction = static_cast<float>(limitCount - usedCount) / limitCount;
    }

    std::string tierLabel;
    auto tierIt = acc.metadata.find("kiroTier");
    if (tierIt != acc.metadata.end() && !tierIt->second.empty()) {
      KiroTier t = kiroTierFromString(tierIt->second);
      switch (t) {
      case KiroTier::Free:    tierLabel = "Free";    break;
      case KiroTier::Pro:     tierLabel = "Pro";     break;
      case KiroTier::ProPlus: tierLabel = "Pro+";    break;
      case KiroTier::Power:   tierLabel = "Power";   break;
      case KiroTier::Unknown: break;
      }
    }

    bucket.note = tierLabel.empty()
                      ? std::to_string(usedCount) + "/" + std::to_string(limitCount) + " requests"
                      : tierLabel + " · " + std::to_string(usedCount) + "/" +
                            std::to_string(limitCount) + " requests";

    result[acc.identifier] = {bucket};
  }

  return result;
}

std::optional<OAuthAccount>
KiroProvider::getAvailableAccountForModel(const std::string &modelId) {
  // Returns the first qualifying candidate (round-robin starting at
  // lastUsedIndex_). For full ordered traversal during retry, stream()
  // calls collectCandidateAccountIndices() instead.
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  if (accounts_.empty()) {
    return std::nullopt;
  }
  auto indices = collectCandidateAccountIndices(modelId);
  if (indices.empty()) {
    return std::nullopt;
  }
  return accounts_[indices.front()];
}

std::vector<int>
KiroProvider::collectCandidateAccountIndices(const std::string &modelId) {
  // Caller holds accountsMutex_.
  std::vector<int> tiered;
  std::vector<int> unknown;
  if (accounts_.empty()) {
    return {};
  }

  const std::string resolved = resolveModelId(modelId);
  const KiroTier required = modelMinimumTier(resolved);
  const int64_t now = nowSeconds();

  for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
    const auto &acc = accounts_[i];
    if (acc.rateLimited && now < acc.backoffUntil) {
      continue;
    }
    KiroTier tier = accountTier(acc);
    if (tier == KiroTier::Unknown) {
      unknown.push_back(i);
    } else if (static_cast<std::uint8_t>(tier) >=
               static_cast<std::uint8_t>(required)) {
      tiered.push_back(i);
    }
  }

  // Round-robin start: rotate the list so we begin at the index after
  // lastUsedIndex_. Tiered candidates first; unknown-tier accounts as a
  // fallback (we let the backend gate them).
  auto rotateBy = [&](std::vector<int> &v) {
    if (v.size() <= 1) return;
    int last = lastUsedIndex_.load(std::memory_order_relaxed);
    int startIdx = 0;
    for (std::size_t k = 0; k < v.size(); ++k) {
      if (v[k] > last) {
        startIdx = static_cast<int>(k);
        break;
      }
    }
    std::rotate(v.begin(), v.begin() + startIdx, v.end());
  };
  rotateBy(tiered);
  rotateBy(unknown);

  std::vector<int> result;
  result.reserve(tiered.size() + unknown.size());
  for (int i : tiered) result.push_back(i);
  for (int i : unknown) result.push_back(i);
  return result;
}

bool KiroProvider::ensureFreshToken(OAuthAccount &acc) {
  // Refresh if expired (within 5 minutes of expiry) and persist on success.
  // Returns false only when refresh genuinely failed.
  if (!isTokenExpired(acc)) {
    return true;
  }
  if (!refreshAccessToken(acc)) {
    return false;
  }
  // Mirror the refreshed token + expiration back into accounts_ so other
  // callers see the new credentials, then persist to disk so subsequent
  // process starts don't re-discover the same expired token.
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  for (auto &existing : accounts_) {
    if (existing.identifier == acc.identifier) {
      existing.accessToken = acc.accessToken;
      existing.refreshToken = acc.refreshToken;
      existing.tokenExpiration = acc.tokenExpiration;
      break;
    }
  }
  saveAccounts();
  return true;
}

KiroProvider::AttemptOutcome KiroProvider::streamOnceForAccount(
    const AgentHistory &history, const ProviderOptions &opts,
    OAuthAccount &acc, const std::string &resolvedModel,
    bool emitTransientErrors,
    std::function<void(const StreamEvent &)> onEvent,
    std::optional<StreamError> *outError) {

  auto record = [&](StreamError err) {
    if (outError) {
      *outError = err;
    }
    if (emitTransientErrors) {
      onEvent(err);
    }
  };

  std::string requestBody = buildCodeWhispererRequest(history, opts.modelId, acc, opts);

  // Optional request-body dump (debug aid): writes the JSON body to a file
  // when FIRMIUS_KIRO_REQUEST_LOG is set.
  if (const char *reqLog = std::getenv("FIRMIUS_KIRO_REQUEST_LOG");
      reqLog && *reqLog) {
    std::ofstream out(reqLog, std::ios::app | std::ios::binary);
    out << "==== request to model=" << opts.modelId
        << " account=" << acc.identifier << " ====\n"
        << requestBody << "\n";
  }

  std::string region = acc.metadata.count("region") ? acc.metadata["region"] : kDefaultRegion;
  std::string apiUrl = buildUrl("https://q.{{region}}.amazonaws.com/generateAssistantResponse", region);
  std::string profileArn = acc.metadata.count("profileArn") ? acc.metadata["profileArn"] : "";

  StreamContext ctx;
  ctx.provider = this;
  ctx.onEvent = &onEvent;
  ctx.abortSignal = opts.abortSignal;
  ctx.resolvedModelId = resolvedModel;

  CURL *curl = curl_easy_init();
  if (!curl) {
    record(StreamError{"Failed to initialize CURL", 500, acc.identifier});
    return AttemptOutcome::OtherFailure;
  }

  struct curl_slist *chunk = nullptr;
  chunk = curl_slist_append(chunk, "Content-Type: application/json");
  chunk = curl_slist_append(chunk, "Accept: application/json");
  chunk = curl_slist_append(chunk, ("Authorization: Bearer " + acc.accessToken).c_str());
  chunk = curl_slist_append(chunk, ("amz-sdk-invocation-id: " + firmius::shared::StringUtil::generateUuid()).c_str());
  chunk = curl_slist_append(chunk, "amz-sdk-request: attempt=1; max=1");
  chunk = curl_slist_append(chunk, "x-amzn-kiro-agent-mode: vibe");
  chunk = curl_slist_append(chunk, "x-amz-user-agent: aws-sdk-js/3.738.0 KiroIDE");
  chunk = curl_slist_append(
      chunk,
      "user-agent: aws-sdk-js/3.738.0 ua/2.1 os/linux#unknown lang/js md/nodejs#22 api/codewhisperer#3.738.0 m/E KiroIDE");
  chunk = curl_slist_append(chunk, "Connection: close");
  if (!profileArn.empty()) {
    chunk = curl_slist_append(chunk, ("x-amzn-profile-arn: " + profileArn).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, apiUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sseWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  onEvent(ProviderWaiting{});

  CURLcode res = curl_easy_perform(curl);

  long httpStatus = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

  curl_slist_free_all(chunk);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    record(StreamError{std::string("curl: ") + curl_easy_strerror(res), 0,
                       acc.identifier});
    return AttemptOutcome::OtherFailure;
  }

  // Helper: produce a StreamError that includes the raw response body so the
  // user sees what AWS actually said instead of a placeholder.
  auto buildHttpError = [&](long status) {
    std::string msg = "HTTP " + std::to_string(status);
    if (!ctx.buffer.empty()) {
      std::string snippet = ctx.buffer;
      if (snippet.size() > 2000) {
        snippet.resize(2000);
        snippet += "...";
      }
      msg += " body=" + snippet;
    }
    return StreamError{msg, static_cast<int>(status), acc.identifier};
  };

  // 401/403: token rejected by AWS even though our local clock said it was
  // valid. The Q endpoint sometimes invalidates tokens before the stored
  // expiration; treat this as auth failure so the caller can refresh + retry.
  if (httpStatus == 401 || httpStatus == 403) {
    record(buildHttpError(httpStatus));
    return AttemptOutcome::AuthFailed;
  }

  if (httpStatus == 429) {
    {
      std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
      for (auto &existing : accounts_) {
        if (existing.identifier == acc.identifier) {
          markAccountRateLimited(existing, 60);
          break;
        }
      }
    }
    record(buildHttpError(httpStatus));
    return AttemptOutcome::RateLimited;
  }

  if (httpStatus >= 400) {
    // 4xx other than auth/rate-limit, or 5xx: surface the real body and let
    // the rotation loop decide whether to retry on another account.
    record(buildHttpError(httpStatus));
    return AttemptOutcome::OtherFailure;
  }

  // Success path — flush trailing buffers, emit metrics, finalize.
  if (!ctx.metricsReceived && !ctx.buffer.empty()) {
    size_t pos = ctx.buffer.find("{\"contextUsagePercentage\":");
    if (pos != std::string::npos) {
      rapidjson::Document doc;
      std::string jsonStr = ctx.buffer.substr(pos);
      size_t endPos = jsonStr.find('}');
      if (endPos != std::string::npos) {
        jsonStr = jsonStr.substr(0, endPos + 1);
        doc.Parse(jsonStr.c_str());
        if (!doc.HasParseError() && doc.HasMember("contextUsagePercentage")) {
          float pct = doc["contextUsagePercentage"].GetFloat();
          ctx.metrics.tokens.contextSize = static_cast<std::uint32_t>(200000 * pct / 100);
        }
      }
    }
  }

  if (!ctx.contentBuffer.empty()) {
    if (ctx.inThinking) {
      onEvent(ThinkingChunk{ctx.contentBuffer, ""});
    } else {
      onEvent(TextChunk{ctx.contentBuffer});
    }
    ctx.contentBuffer.clear();
  }

  onEvent(ctx.metrics);
  onEvent(StreamDone{ctx.doneReceived ? StopReason::ToolUse : StopReason::Stop});
  return AttemptOutcome::Success;
}

std::optional<OAuthAccount> KiroProvider::getAvailableAccount(const std::optional<std::string> &modelId) {
  if (modelId.has_value() && !modelId->empty()) {
    if (auto chosen = getAvailableAccountForModel(*modelId)) {
      return chosen;
    }
  }
  return BaseOAuthProvider::getAvailableAccount(modelId);
}

void KiroProvider::stream(const AgentHistory &history, const ProviderOptions &opts,
                          std::function<void(const StreamEvent &)> onEvent) {
  const std::string resolvedModel = resolveModelId(opts.modelId);

  // Build the ordered candidate list once, up front. We rotate through it on
  // failures (auth / rate limit / transient HTTP errors) until we either
  // succeed or exhaust every account.
  std::vector<int> candidateIndices;
  std::vector<OAuthAccount> candidates;
  {
    std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
    candidateIndices = collectCandidateAccountIndices(resolvedModel);
    candidates.reserve(candidateIndices.size());
    for (int i : candidateIndices) {
      candidates.push_back(accounts_[i]);
    }
  }

  if (candidates.empty()) {
    std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
    if (!accounts_.empty()) {
      const KiroTier required = modelMinimumTier(resolvedModel);
      if (required > KiroTier::Free) {
        onEvent(StreamError{
            std::string("Model '") + resolvedModel +
                "' requires Kiro " +
                (required == KiroTier::Power ? "Power"
                 : required == KiroTier::ProPlus ? "Pro+"
                                                 : "Pro") +
                ". None of your connected Kiro accounts is on a qualifying"
                " plan — connect a Pro/Pro+/Power account or pick a"
                " Free-tier model (auto, claude-sonnet-4.5, qwen3-coder-next,"
                " glm-5, deepseek-3.2, minimax-m2.1, minimax-m2.5).",
            403, ""});
        onEvent(StreamDone{StopReason::Error});
        return;
      }
    }
    onEvent(StreamError{"No Kiro account available. Run /connect kiro to authenticate.", 401, ""});
    onEvent(StreamDone{StopReason::Error});
    return;
  }

  // Rotation loop. For each candidate:
  //   1) Refresh its access token if expired.
  //   2) Make the streaming request.
  //   3) On success → done.
  //   4) On AuthFailed → if we haven't tried refreshing yet, refresh once and
  //      retry the same account; otherwise rotate to the next.
  //   5) On RateLimited → mark the account, rotate.
  //   6) On OtherFailure → rotate.
  //
  // We only emit the final StreamError + StreamDone on the *last* candidate
  // failure; intermediate errors are suppressed so the caller doesn't see
  // them as terminal.

  std::optional<StreamError> lastError;
  bool succeeded = false;

  for (std::size_t idx = 0; idx < candidates.size(); ++idx) {
    OAuthAccount acc = candidates[idx];

    // Step 1: proactive refresh.
    if (!ensureFreshToken(acc)) {
      lastError = StreamError{"refresh failed for " + acc.identifier, 401,
                              acc.identifier};
      continue;
    }

    bool retriedAfterAuthFailure = false;
    while (true) {
      // Forwarding shim: passes through chunks/metrics, captures any
      // StreamError into `attemptError` (we surface it later as `lastError`),
      // and suppresses StreamDone — we re-emit a single canonical Done at
      // the very end so the caller sees exactly one terminal event.
      std::optional<StreamError> attemptError;
      AttemptOutcome outcome;
      auto forward = [&](const StreamEvent &ev) {
        if (std::holds_alternative<StreamError>(ev)) {
          // streamOnceForAccount also writes the same error into outError;
          // capturing here is a defensive backup in case of future code
          // paths that emit StreamError without going through `record`.
          if (!attemptError) {
            attemptError = std::get<StreamError>(ev);
          }
          return;
        }
        if (std::holds_alternative<StreamDone>(ev)) {
          return;
        }
        onEvent(ev);
      };

      outcome = streamOnceForAccount(history, opts, acc, resolvedModel,
                                     /*emitTransientErrors=*/false, forward,
                                     &attemptError);

      if (outcome == AttemptOutcome::Success) {
        // streamOnceForAccount already emitted metrics + (suppressed) Done
        // via `forward`. Re-emit a canonical Done now and advance the
        // round-robin pointer.
        lastUsedIndex_.store(candidateIndices[idx], std::memory_order_relaxed);
        onEvent(StreamDone{StopReason::Stop});
        succeeded = true;
        break;
      }

      if (outcome == AttemptOutcome::AuthFailed && !retriedAfterAuthFailure) {
        // Force a refresh and retry once on this account. AWS sometimes
        // invalidates tokens early; a single refresh usually clears it.
        retriedAfterAuthFailure = true;
        acc.tokenExpiration = 0;
        if (!ensureFreshToken(acc)) {
          if (attemptError) {
            lastError = attemptError;
          } else {
            lastError = StreamError{"refresh failed after 401/403 for " +
                                        acc.identifier,
                                    401, acc.identifier};
          }
          break;
        }
        continue;
      }

      // Non-recoverable on this account — preserve the real error and rotate.
      if (attemptError) {
        lastError = attemptError;
      }
      break;
    }

    if (succeeded) break;
  }

  if (!succeeded) {
    if (lastError) {
      onEvent(*lastError);
    } else {
      onEvent(StreamError{"All Kiro accounts failed without a captured error",
                          0, ""});
    }
    onEvent(StreamDone{StopReason::Error});
  }
}

std::string KiroProvider::buildCodeWhispererRequest(
    const AgentHistory &history,
    const std::string &modelId,
    const OAuthAccount &acc,
    const ProviderOptions &opts) {

  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();

  std::string conversationId = firmius::shared::StringUtil::generateUuid();
  std::string resolvedModel = resolveModelId(modelId);

  // Build conversation state
  rapidjson::Value conversationState(rapidjson::kObjectType);
  conversationState.AddMember("chatTriggerType", "MANUAL", alloc);
  conversationState.AddMember("conversationId", rapidjson::Value(conversationId.c_str(), alloc), alloc);

  // Build history from turns
  rapidjson::Value historyArr(rapidjson::kArrayType);
  
  for (size_t turnIdx = 0; turnIdx < history.turns.size(); ++turnIdx) {
    const auto &turn = history.turns[turnIdx];
    bool isLastTurn = (turnIdx == history.turns.size() - 1);
    
    for (const auto &msg : turn.messages) {
      // Skip the last user message - it goes in currentMessage
      if (isLastTurn && msg.role == Role::User) continue;
      
      if (msg.role == Role::User) {
        rapidjson::Value histEntry(rapidjson::kObjectType);
        rapidjson::Value uim(rapidjson::kObjectType);
        
        std::string content;
        for (const auto &part : msg.content) {
          if (auto *txt = std::get_if<TextContent>(&part)) {
            content = txt->text;
          }
        }
        
        uim.AddMember("content", rapidjson::Value(content.c_str(), alloc), alloc);
        uim.AddMember("modelId", rapidjson::Value(resolvedModel.c_str(), alloc), alloc);
        uim.AddMember("origin", "AI_EDITOR", alloc);
        histEntry.AddMember("userInputMessage", uim, alloc);
        historyArr.PushBack(histEntry, alloc);
      }
      else if (msg.role == Role::Assistant) {
        rapidjson::Value histEntry(rapidjson::kObjectType);
        rapidjson::Value arm(rapidjson::kObjectType);
        
        std::string content;
        rapidjson::Value toolUses(rapidjson::kArrayType);
        
        for (const auto &part : msg.content) {
          if (auto *txt = std::get_if<TextContent>(&part)) {
            content += txt->text;
          }
          else if (auto *tc = std::get_if<ToolCallContent>(&part)) {
            rapidjson::Value tu(rapidjson::kObjectType);
            tu.AddMember("toolUseId", rapidjson::Value(tc->id.c_str(), alloc), alloc);
            tu.AddMember("name", rapidjson::Value(tc->name.c_str(), alloc), alloc);
            rapidjson::Document inputDoc;
            inputDoc.Parse(tc->args.c_str());
            if (!inputDoc.HasParseError() &&
                (inputDoc.IsObject() || inputDoc.IsArray())) {
              tu.AddMember("input", rapidjson::Value(inputDoc, alloc), alloc);
            } else {
              tu.AddMember("input", rapidjson::Value(tc->args.c_str(), alloc),
                           alloc);
            }
            toolUses.PushBack(tu, alloc);
          }
        }
        
        arm.AddMember("content", rapidjson::Value(content.c_str(), alloc), alloc);
        if (toolUses.Size() > 0) {
          arm.AddMember("toolUses", toolUses, alloc);
        }
        histEntry.AddMember("assistantResponseMessage", arm, alloc);
        historyArr.PushBack(histEntry, alloc);
      }
      else if (msg.role == Role::ToolResult) {
        rapidjson::Value histEntry(rapidjson::kObjectType);
        rapidjson::Value uim(rapidjson::kObjectType);
        rapidjson::Value ctx(rapidjson::kObjectType);
        rapidjson::Value toolResults(rapidjson::kArrayType);
        
        for (const auto &part : msg.content) {
          if (auto *tr = std::get_if<ToolResultContent>(&part)) {
            rapidjson::Value trVal(rapidjson::kObjectType);
            rapidjson::Value trContent(rapidjson::kArrayType);
            
            rapidjson::Value txtVal(rapidjson::kObjectType);
            txtVal.AddMember("text", rapidjson::Value(tr->result.c_str(), alloc), alloc);
            trContent.PushBack(txtVal, alloc);
            
            trVal.AddMember("content", trContent, alloc);
            trVal.AddMember("status", rapidjson::Value(tr->success ? "success" : "error", alloc), alloc);
            trVal.AddMember("toolUseId", rapidjson::Value(tr->toolCallId.c_str(), alloc), alloc);
            toolResults.PushBack(trVal, alloc);
          }
        }
        
        ctx.AddMember("toolResults", toolResults, alloc);
        uim.AddMember("content", "Tool results provided.", alloc);
        uim.AddMember("modelId", rapidjson::Value(resolvedModel.c_str(), alloc), alloc);
        uim.AddMember("origin", "AI_EDITOR", alloc);
        uim.AddMember("userInputMessageContext", ctx, alloc);
        histEntry.AddMember("userInputMessage", uim, alloc);
        historyArr.PushBack(histEntry, alloc);
      }
    }
  }

  if (historyArr.Size() > 0) {
    conversationState.AddMember("history", historyArr, alloc);
  }

  // Build current message from last user message
  rapidjson::Value currentMessage(rapidjson::kObjectType);
  rapidjson::Value userInputMessage(rapidjson::kObjectType);

  std::string userContent = "Continue";
  std::string systemPrompt;
  
  if (!history.turns.empty()) {
    const auto &lastTurn = history.turns.back();
    for (const auto &msg : lastTurn.messages) {
      if (msg.role == Role::System) {
        for (const auto &part : msg.content) {
          if (auto *txt = std::get_if<TextContent>(&part)) {
            systemPrompt = txt->text;
          }
        }
      }
      else if (msg.role == Role::User) {
        for (const auto &part : msg.content) {
          if (auto *txt = std::get_if<TextContent>(&part)) {
            userContent = txt->text;
          }
        }
      }
    }
  }

  // Prepend system prompt if present
  if (!systemPrompt.empty()) {
    userContent = systemPrompt + "\n\n" + userContent;
  }

  userInputMessage.AddMember("content", rapidjson::Value(userContent.c_str(), alloc), alloc);
  userInputMessage.AddMember("modelId", rapidjson::Value(resolvedModel.c_str(), alloc), alloc);
  userInputMessage.AddMember("origin", "AI_EDITOR", alloc);

  // Add tools (always, since we inject a synthetic `thinking` tool regardless
  // of what the caller asked for — this is how Kiro's own CLI gets every
  // model to emit reasoning, including ones that don't have a wire-protocol
  // thinking opt-in).
  {
    rapidjson::Value ctx(rapidjson::kObjectType);
    rapidjson::Value toolsArr(rapidjson::kArrayType);

    for (const auto &tool : opts.tools) {
      // Defensive: don't double-inject if the caller is already passing a
      // tool literally named "thinking".
      if (tool.name == "thinking") {
        continue;
      }
      rapidjson::Value toolSpec(rapidjson::kObjectType);
      rapidjson::Value ts(rapidjson::kObjectType);
      ts.AddMember("name", rapidjson::Value(tool.name.c_str(), alloc), alloc);
      ts.AddMember("description",
                   rapidjson::Value(tool.description.c_str(), alloc), alloc);

      rapidjson::Document schemaDoc;
      schemaDoc.Parse(tool.inputSchema.c_str());
      if (!schemaDoc.HasParseError()) {
        rapidjson::Value schema(rapidjson::kObjectType);
        schema.AddMember("json", rapidjson::Value(schemaDoc, alloc), alloc);
        ts.AddMember("inputSchema", schema, alloc);
      }

      toolSpec.AddMember("toolSpecification", ts, alloc);
      toolsArr.PushBack(toolSpec, alloc);
    }

    // Synthetic `thinking` tool — every Kiro model (including Sonnet 4.5,
    // Haiku 4.5, GLM-5, MiniMax, DeepSeek, Qwen) will call this tool when
    // it's offered. The provider intercepts these calls and translates them
    // into ThinkingChunk events; the harness never sees them as tool calls.
    {
      rapidjson::Value thinkSpec(rapidjson::kObjectType);
      rapidjson::Value ts(rapidjson::kObjectType);
      ts.AddMember("name", "thinking", alloc);
      ts.AddMember(
          "description",
          "Thinking is an internal reasoning mechanism improving the quality "
          "of complex tasks by breaking their atomic actions down; use it "
          "specifically for multi-step problems requiring step-by-step "
          "dependencies, reasoning through multiple constraints, synthesizing "
          "results from previous tool calls, planning intricate sequences of "
          "actions, troubleshooting complex errors, or making decisions "
          "involving multiple trade-offs. Avoid using it for straightforward "
          "tasks, basic information retrieval, summaries; always clearly "
          "define the reasoning challenge, structure thoughts explicitly, "
          "consider multiple perspectives, and summarize key insights before "
          "important decisions or complex tool interactions.",
          alloc);
      rapidjson::Value schema(rapidjson::kObjectType);
      rapidjson::Document inner;
      inner.Parse(R"({"type":"object","properties":{"thought":{"type":"string","description":"A reflective note or intermediate reasoning step. This is shown to the user as your reasoning trace."}},"required":["thought"]})");
      schema.AddMember("json", rapidjson::Value(inner, alloc), alloc);
      ts.AddMember("inputSchema", schema, alloc);
      thinkSpec.AddMember("toolSpecification", ts, alloc);
      toolsArr.PushBack(thinkSpec, alloc);
    }

    ctx.AddMember("tools", toolsArr, alloc);
    userInputMessage.AddMember("userInputMessageContext", ctx, alloc);
  }

  currentMessage.AddMember("userInputMessage", userInputMessage, alloc);
  conversationState.AddMember("currentMessage", currentMessage, alloc);

  doc.AddMember("conversationState", conversationState, alloc);

  // Add profile ARN if present
  auto profileArnIt = acc.metadata.find("profileArn");
  std::string profileArn = (profileArnIt != acc.metadata.end()) ? profileArnIt->second : "";
  if (!profileArn.empty()) {
    doc.AddMember("profileArn", rapidjson::Value(profileArn.c_str(), alloc), alloc);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  return buffer.GetString();
}

void KiroProvider::generateSummary(const std::string &modelId,
                                   const AgentHistory &history,
                                   const std::string & /*compactionPrompt*/,
                                   std::function<void(const StreamEvent &)> onEvent,
                                   std::atomic<bool> *abortSignal) {
  ProviderOptions opts;
  opts.modelId = modelId;
  opts.abortSignal = abortSignal;
  stream(history, opts, onEvent);
}

size_t KiroProvider::sseWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *ctx = static_cast<StreamContext *>(userdata);
  size_t totalSize = size * nmemb;

  if (ctx->abortSignal && ctx->abortSignal->load(std::memory_order_relaxed)) {
    return 0;
  }

  maybeLogRawKiroChunk(ptr, totalSize);
  ctx->buffer.append(ptr, totalSize);

  // Parse AWS event stream format by consuming only the unread suffix.
  while (true) {
    size_t jsonStart = std::string::npos;
    const char *markers[] = {
        "{\"content\":",
        "{\"text\":",
        "{\"thinking\":",
        "{\"reasoning\":",
        "{\"reasoning_content\":",
        "{\"name\":",
        "{\"input\":",
        "{\"stop\":",
        "{\"contextUsagePercentage\":",
        "{\"followupPrompt\":",
        "{\"type\":",
        "{\"choices\":",
        "{\"delta\":",
        "{\"event\":",
        "{\"usage\":",
        "{\"toolUseId\":",
        "{\"unit\":",
        // No bare-brace fallback: AWS event-stream framing CRC bytes
        // routinely contain `{` bytes that aren't real JSON. Letting the
        // brace-matcher follow them swallowed real subsequent JSON
        // payloads. Every shape we actually want is already covered by an
        // explicit key prefix above.
    };
    
    for (const char *marker : markers) {
      size_t pos = ctx->buffer.find(marker, ctx->readOffset);
      if (pos != std::string::npos && (jsonStart == std::string::npos || pos < jsonStart)) {
        jsonStart = pos;
      }
    }
    
    if (jsonStart == std::string::npos) break;

    // Find matching closing brace
    int braceCount = 0;
    size_t jsonEnd = std::string::npos;
    bool inString = false;
    
    for (size_t i = jsonStart; i < ctx->buffer.size(); ++i) {
      char c = ctx->buffer[i];
      if (c == '"' && (i == 0 || ctx->buffer[i-1] != '\\')) {
        inString = !inString;
      } else if (!inString) {
        if (c == '{') braceCount++;
        else if (c == '}') {
          braceCount--;
          if (braceCount == 0) {
            jsonEnd = i;
            break;
          }
        }
      }
    }

    if (jsonEnd == std::string::npos) break;

    std::string jsonStr = ctx->buffer.substr(jsonStart, jsonEnd - jsonStart + 1);
    ctx->readOffset = jsonEnd + 1;

    rapidjson::Document doc;
    doc.Parse(jsonStr.c_str());
    if (doc.HasParseError()) continue;

    // 1) Bedrock content_block_* events (used by GLM-5, MiniMax, DeepSeek,
    //    Qwen, Haiku 4.5 thinking deltas, and Anthropic 4.x reasoning).
    //    The dispatcher sets ctx.thinkingExtracted internally so any
    //    subsequent in-band text is emitted directly.
    if (dispatchBedrockEventChunk(*ctx, doc)) {
      continue;
    }

    // 2) OpenAI-style chat completion chunks (DeepSeek/Qwen/MiniMax forward
    //    in this shape via the Kiro Q endpoint).
    if (dispatchOpenAIChoicesChunk(*ctx, doc)) {
      continue;
    }

    // 3) Anthropic-classic shape used by Sonnet 4.0/4.5/4.6 and Opus 4.x via
    //    the Q endpoint. Below is the original parser, with additional
    //    fallbacks for `reasoning_content` and nested `delta.thinking`.

    // Thinking/reasoning delta (sometimes sent out-of-band)
    if (doc.HasMember("thinking")) {
      const std::string delta = serializeJsonValue(doc["thinking"]);
      if (!delta.empty()) {
        (*ctx->onEvent)(ThinkingChunk{delta, ""});
      }
    } else if (doc.HasMember("reasoning")) {
      const std::string delta = serializeJsonValue(doc["reasoning"]);
      if (!delta.empty()) {
        (*ctx->onEvent)(ThinkingChunk{delta, ""});
      }
    } else if (doc.HasMember("reasoning_content")) {
      const std::string delta = serializeJsonValue(doc["reasoning_content"]);
      if (!delta.empty()) {
        (*ctx->onEvent)(ThinkingChunk{delta, ""});
      }
    } else if (doc.HasMember("text") && doc["text"].IsString() &&
               !doc.HasMember("content") && !doc.HasMember("modelId")) {
      // The Q endpoint surfaces internal reasoning for Opus 4.7 and the
      // MiniMax models as bare {"text":"..."} chunks (no modelId, no
      // content). User-visible answer chunks carry both "content" and
      // "modelId", so we use the absence of those siblings to disambiguate.
      const std::string delta = doc["text"].GetString();
      if (!delta.empty()) {
        (*ctx->onEvent)(ThinkingChunk{delta, ""});
        // Like the Bedrock/OpenAI dispatchers, this dialect never wraps
        // text in inline <thinking> tags — clear the holdback so any
        // subsequent {"content":...} chunks emit their TextChunk directly.
        ctx->thinkingExtracted = true;
      }
    } else if (doc.HasMember("delta") && doc["delta"].IsObject()) {
      const auto &d = doc["delta"];
      if (d.HasMember("thinking") && d["thinking"].IsString()) {
        const std::string thinking = d["thinking"].GetString();
        if (!thinking.empty()) {
          (*ctx->onEvent)(ThinkingChunk{thinking, ""});
        }
      } else if (d.HasMember("reasoning_content") &&
                 d["reasoning_content"].IsString()) {
        const std::string thinking = d["reasoning_content"].GetString();
        if (!thinking.empty()) {
          (*ctx->onEvent)(ThinkingChunk{thinking, ""});
        }
      }
    }

    // Content delta
    if (doc.HasMember("content") && !doc.HasMember("followupPrompt")) {
      std::string content = serializeJsonValue(doc["content"]);
      emitKiroContentDelta(*ctx, content);
    }
    // Tool use
    else if (doc.HasMember("name") && doc.HasMember("toolUseId")) {
      std::string name = serializeJsonValue(doc["name"]);
      std::string toolUseId = serializeJsonValue(doc["toolUseId"]);
      std::string input =
          doc.HasMember("input") ? serializeJsonValue(doc["input"]) : "";
      const bool sameTool = (ctx->activeToolUseId == toolUseId);
      if (!sameTool) {
        ctx->activeToolName.clear();
        ctx->activeToolArgs.clear();
        ctx->activeToolFinalized = false;
      }
      ctx->activeToolUseId = toolUseId;
      if (!name.empty()) {
        ctx->activeToolName = name;
      }
      if (!input.empty()) {
        if (ctx->activeToolArgs.empty()) {
          ctx->activeToolArgs = input;
        } else if (input.rfind(ctx->activeToolArgs, 0) == 0) {
          ctx->activeToolArgs = input;
        } else {
          ctx->activeToolArgs += input;
        }
      }

      (*ctx->onEvent)(ToolCallChunk{
          toolUseId, std::numeric_limits<std::uint32_t>::max(),
          sameTool ? "" : name, input});

      if (doc.HasMember("stop") && doc["stop"].IsBool() && doc["stop"].GetBool()) {
        if (!ctx->activeToolFinalized && !ctx->activeToolName.empty() &&
            !ctx->activeToolArgs.empty()) {
          ctx->activeToolFinalized = true;
          (*ctx->onEvent)(ToolCall{ctx->activeToolUseId,
                                   std::numeric_limits<std::uint32_t>::max(),
                                   ctx->activeToolName, ctx->activeToolArgs});
        }
        ctx->doneReceived = true;
      }
    }
    else if (doc.HasMember("input") && !doc.HasMember("name") &&
             !ctx->activeToolUseId.empty()) {
      const std::string inputDelta = serializeJsonValue(doc["input"]);
      if (ctx->activeToolArgs.empty()) {
        ctx->activeToolArgs = inputDelta;
      } else {
        ctx->activeToolArgs += inputDelta;
      }
      (*ctx->onEvent)(ToolCallChunk{
          ctx->activeToolUseId, std::numeric_limits<std::uint32_t>::max(), "",
          inputDelta});
    }
    // Context usage (metrics)
    else if (doc.HasMember("contextUsagePercentage")) {
      float pct = doc["contextUsagePercentage"].GetFloat();
      ctx->metrics.tokens.contextSize = static_cast<std::uint32_t>(200000 * pct / 100);
      ctx->metricsReceived = true;
    }
    // Token caching: Kiro's Q Developer endpoint does not currently
    // surface Bedrock cache fields, but it's a proxy in front of Bedrock
    // — defensively parse both Converse-API style (camelCase) and
    // InvokeModel-API style (snake_case) so cache hits land if AWS ever
    // exposes them. No-ops today; cheap to keep.
    else if (doc.HasMember("usage") && doc["usage"].IsObject()) {
      const auto &u = doc["usage"];
      if (u.HasMember("cacheReadInputTokens") && u["cacheReadInputTokens"].IsUint()) {
        ctx->metrics.tokens.cacheRead = u["cacheReadInputTokens"].GetUint();
      } else if (u.HasMember("cache_read_input_tokens") && u["cache_read_input_tokens"].IsUint()) {
        ctx->metrics.tokens.cacheRead = u["cache_read_input_tokens"].GetUint();
      }
      if (u.HasMember("cacheWriteInputTokens") && u["cacheWriteInputTokens"].IsUint()) {
        ctx->metrics.tokens.cacheWrite = u["cacheWriteInputTokens"].GetUint();
      } else if (u.HasMember("cache_creation_input_tokens") && u["cache_creation_input_tokens"].IsUint()) {
        ctx->metrics.tokens.cacheWrite = u["cache_creation_input_tokens"].GetUint();
      }
      if (u.HasMember("inputTokens") && u["inputTokens"].IsUint()) {
        ctx->metrics.tokens.prompt = u["inputTokens"].GetUint();
      } else if (u.HasMember("input_tokens") && u["input_tokens"].IsUint()) {
        ctx->metrics.tokens.prompt = u["input_tokens"].GetUint();
      }
      if (u.HasMember("outputTokens") && u["outputTokens"].IsUint()) {
        ctx->metrics.tokens.completion = u["outputTokens"].GetUint();
      } else if (u.HasMember("output_tokens") && u["output_tokens"].IsUint()) {
        ctx->metrics.tokens.completion = u["output_tokens"].GetUint();
      }
      ctx->metricsReceived = true;
    }
    // Stop signal
    else if (doc.HasMember("stop") && !doc.HasMember("name")) {
      if (!ctx->activeToolUseId.empty() && !ctx->activeToolFinalized &&
          !ctx->activeToolName.empty() && !ctx->activeToolArgs.empty()) {
        ctx->activeToolFinalized = true;
        (*ctx->onEvent)(ToolCall{ctx->activeToolUseId,
                                 std::numeric_limits<std::uint32_t>::max(),
                                 ctx->activeToolName, ctx->activeToolArgs});
      }
      ctx->doneReceived = true;
    }
  }

  if (ctx->readOffset > 0 &&
      (ctx->readOffset == ctx->buffer.size() ||
       ctx->readOffset > 65536)) {
    ctx->buffer.erase(0, ctx->readOffset);
    ctx->readOffset = 0;
  }

  return totalSize;
}

// ============================================================================
// KiroOAuthWizard Implementation
// ============================================================================

class KiroOAuthWizard : public OAuthWizard {
public:
  explicit KiroOAuthWizard(KiroProvider *provider) : provider_(provider) {
    prompt_ = authMethodPrompt();
  }

  ~KiroOAuthWizard() override {
    stopPolling_ = true;
    if (pollingThread_.joinable()) {
      pollingThread_.join();
    }
  }

  std::optional<WizardPrompt> nextPrompt() override {
    if (prompt_.empty()) return std::nullopt;

    WizardPrompt prompt;
    prompt.message = prompt_;
    switch (state_) {
    case State::ChooseAuthMethod:
      prompt.choices = {
          {"Builder ID (free)", "1"},
          {"IAM Identity Center", "2"},
          {"Kiro Desktop / kiro-cli import", "3"},
      };
      prompt.allowFreeformInput = false;
      prompt.submitLabel = "Choose Option";
      break;
    case State::ChooseIdcFlavor:
      prompt.choices = {
          {"Builder ID start URL", "1"},
          {"Custom IAM Identity Center start URL", "2"},
      };
      prompt.allowFreeformInput = false;
      prompt.submitLabel = "Choose Option";
      break;
    case State::EnterIdcStartUrl:
      prompt.placeholder = "https://your-domain.awsapps.com/start";
      prompt.submitLabel = "Continue";
      break;
    case State::EnterIdcRegion:
      prompt.placeholder = kDefaultRegion;
      prompt.allowEmptyInput = true;
      prompt.submitLabel = "Continue";
      break;
    case State::WaitingForOAuthCompletion:
      prompt.allowFreeformInput = false;
      prompt.allowEmptyInput = true;
      prompt.submitLabel = "Open Browser / Wait";
      break;
    case State::ReadyToImportDesktop:
      prompt.allowFreeformInput = false;
      prompt.allowEmptyInput = true;
      prompt.submitLabel = "Import Session";
      break;
    case State::Idle:
      return std::nullopt;
    }
    return prompt;
  }

  void submitAnswer(const std::string &answer) override {
    if (state_ == State::WaitingForOAuthCompletion) {
      if (pollingThread_.joinable()) {
        pollingThread_.join();
      }
      return;
    }

    std::string trimmed = StringUtil::trim(answer);
    switch (state_) {
    case State::ChooseAuthMethod:
      handleAuthMethodChoice(trimmed);
      return;
    case State::ChooseIdcFlavor:
      handleIdcFlavorChoice(trimmed);
      return;
    case State::EnterIdcStartUrl:
      handleStartUrl(trimmed);
      return;
    case State::EnterIdcRegion:
      handleRegion(trimmed);
      return;
    case State::Idle:
      return;
    case State::ReadyToImportDesktop:
      prompt_.clear();
      state_ = State::Idle;
      return;
    case State::WaitingForOAuthCompletion:
      if (pollingThread_.joinable()) {
        pollingThread_.join();
      }
      return;
    }
  }

  bool isComplete() const override {
    return isComplete_.load();
  }

  bool finalizeExchange(std::string &outErrorMessage) override {
    if (authMethod_ == "desktop" && importRequested_) {
      return importFromKiroCli(outErrorMessage);
    }

    if (!tokenReceived_.load()) {
      outErrorMessage = errorMessage_.empty() ? "OAuth authorization not completed" : errorMessage_;
      return false;
    }

    OAuthAccount acc;
    acc.accessToken = accessToken_;
    acc.refreshToken = refreshToken_;
    acc.tokenExpiration = tokenExpiration_;
    acc.identifier = stableKiroAccountIdentifier(authMethod_, email_, clientId_,
                                                 profileArn_);

    acc.metadata["authMethod"] = authMethod_;
    acc.metadata["region"] = region_;
    if (!email_.empty()) acc.metadata["email"] = email_;
    if (!clientId_.empty()) acc.metadata["clientId"] = clientId_;
    if (!clientSecret_.empty()) acc.metadata["clientSecret"] = clientSecret_;
    if (!profileArn_.empty()) acc.metadata["profileArn"] = profileArn_;
    if (!startUrl_.empty()) acc.metadata["startUrl"] = startUrl_;

    provider_->addAccount(acc);
    return true;
  }

  std::string getFinalMessage() const override {
    if (authMethod_ == "desktop" && importRequested_) {
      return "Imported Kiro Desktop session from the local kiro-cli database.";
    }
    return "Successfully authenticated with Kiro!";
  }

private:
  enum class State {
    ChooseAuthMethod,
    ChooseIdcFlavor,
    EnterIdcStartUrl,
    EnterIdcRegion,
    WaitingForOAuthCompletion,
    ReadyToImportDesktop,
    Idle
  };

  static std::string authMethodPrompt() {
    return "How would you like to authenticate with Kiro?\n"
           "1) Builder ID (free)\n"
           "2) IAM Identity Center\n"
           "3) Kiro Desktop / kiro-cli import\n\n"
           "Enter 1, 2, or 3:";
  }

  void setErrorAndComplete(const std::string &message) {
    errorMessage_ = message;
    prompt_ = message;
    isComplete_.store(true);
    state_ = State::Idle;
  }

  void handleAuthMethodChoice(const std::string &choice) {
    if (choice == "1") {
      authMethod_ = "idc";
      region_ = kDefaultRegion;
      startUrl_ = "https://view.awsapps.com/start";
      state_ = State::WaitingForOAuthCompletion;
      startDeviceFlow();
      return;
    }
    if (choice == "2") {
      authMethod_ = "idc";
      state_ = State::ChooseIdcFlavor;
      prompt_ = "Use the Builder ID start URL or provide your IAM Identity Center start URL?\n"
                "1) Builder ID start URL\n"
                "2) Custom IAM Identity Center start URL\n\n"
                "Enter 1 or 2:";
      return;
    }
    if (choice == "3") {
      authMethod_ = "desktop";
      importRequested_ = true;
      state_ = State::ReadyToImportDesktop;
      isComplete_.store(true);
      prompt_ = "Importing from your local kiro-cli session.\n"
                "Firmius will read ~/.local/share/kiro-cli/data.sqlite3 (or KIROCLI_DB_PATH) when you confirm this wizard.";
      return;
    }
    prompt_ = authMethodPrompt();
  }

  void handleIdcFlavorChoice(const std::string &choice) {
    if (choice == "1") {
      startUrl_ = "https://view.awsapps.com/start";
      prompt_ = "Enter the AWS region for this Identity Center session\n"
                "(press Enter for us-east-1):";
      state_ = State::EnterIdcRegion;
      return;
    }
    if (choice == "2") {
      prompt_ = "Enter your IAM Identity Center start URL:";
      state_ = State::EnterIdcStartUrl;
      return;
    }
    prompt_ = "Use the Builder ID start URL or provide your IAM Identity Center start URL?\n"
              "1) Builder ID start URL\n"
              "2) Custom IAM Identity Center start URL\n\n"
              "Enter 1 or 2:";
  }

  void handleStartUrl(const std::string &value) {
    if (value.empty()) {
      prompt_ = "Enter your IAM Identity Center start URL:";
      return;
    }
    startUrl_ = value;
    prompt_ = "Enter the AWS region for this Identity Center session\n"
              "(press Enter for us-east-1):";
    state_ = State::EnterIdcRegion;
  }

  void handleRegion(const std::string &value) {
    region_ = value.empty() ? kDefaultRegion : value;
    state_ = State::WaitingForOAuthCompletion;
    startDeviceFlow();
  }

  bool importFromKiroCli(std::string &outErrorMessage) {
    std::string dbPath = kiroCliDbPath();
    if (dbPath.empty()) {
      outErrorMessage = "Could not determine the kiro-cli database path.";
      return false;
    }
    if (!std::filesystem::exists(dbPath)) {
      outErrorMessage = "No kiro-cli database found at: " + dbPath;
      return false;
    }

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
      if (db) sqlite3_close(db);
      outErrorMessage = "Failed to open kiro-cli database: " + dbPath;
      return false;
    }
    sqlite3_exec(db, "PRAGMA busy_timeout = 5000;", nullptr, nullptr, nullptr);

    std::string deviceRegistrationJson;
    readSqliteText(db,
                   "SELECT value FROM auth_kv WHERE key LIKE ? LIMIT 1;",
                   "%device-registration%",
                   deviceRegistrationJson);
    std::string activeProfileState;
    readSqliteText(db,
                   "SELECT value FROM state WHERE key = ? LIMIT 1;",
                   "api.codewhisperer.profile",
                   activeProfileState);

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT key, value FROM auth_kv WHERE key LIKE '%:token';", -1, &stmt, nullptr) != SQLITE_OK) {
      sqlite3_close(db);
      outErrorMessage = "Failed to read tokens from the kiro-cli database.";
      return false;
    }

    std::string chosenKey;
    std::string chosenValue;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
      const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
      if (!key || !value) {
        continue;
      }
      std::string keyStr = key;
      if (keyStr.find("social") != std::string::npos) {
        chosenKey = keyStr;
        chosenValue = value;
        break;
      }
      if (chosenKey.empty()) {
        chosenKey = keyStr;
        chosenValue = value;
      }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (chosenValue.empty()) {
      outErrorMessage = "No Kiro Desktop or IDC tokens were found in the local kiro-cli database.";
      return false;
    }

    rapidjson::Document tokenDoc = parseJsonObject(chosenValue);
    if (!tokenDoc.IsObject()) {
      outErrorMessage = "The imported kiro-cli token row is not valid JSON.";
      return false;
    }

    const bool importedIdc = chosenKey.find("odic") != std::string::npos;
    authMethod_ = importedIdc ? "idc" : "desktop";

    accessToken_ = jsonStringMember(tokenDoc, {"access_token", "accessToken"});
    refreshToken_ = jsonStringMember(tokenDoc, {"refresh_token", "refreshToken"});
    if (refreshToken_.empty()) {
      outErrorMessage = "The imported kiro-cli session does not contain a refresh token.";
      return false;
    }

    clientId_ = jsonStringMember(tokenDoc, {"client_id", "clientId"});
    clientSecret_ = jsonStringMember(tokenDoc, {"client_secret", "clientSecret"});
    if (importedIdc && (clientId_.empty() || clientSecret_.empty()) && !deviceRegistrationJson.empty()) {
      rapidjson::Document registrationDoc = parseJsonObject(deviceRegistrationJson);
      clientId_ = clientId_.empty() ? findClientCredsRecursive(registrationDoc, false) : clientId_;
      clientSecret_ = clientSecret_.empty() ? findClientCredsRecursive(registrationDoc, true) : clientSecret_;
    }

    startUrl_ = jsonStringMember(tokenDoc, {"start_url", "startUrl"});
    profileArn_ = jsonStringMember(tokenDoc, {"profile_arn", "profileArn"});
    if (profileArn_.empty() && !activeProfileState.empty()) {
      profileArn_ = extractProfileArnFromStateValue(activeProfileState);
    }

    std::string importedRegion = jsonStringMember(tokenDoc, {"region"});
    region_ = importedRegion.empty() ? kDefaultRegion : importedRegion;

    tokenExpiration_ = nowSeconds() + 3600;
    if (tokenDoc.HasMember("expires_at")) {
      if (tokenDoc["expires_at"].IsInt64()) {
        auto value = tokenDoc["expires_at"].GetInt64();
        tokenExpiration_ = value > 10000000000LL ? value / 1000 : value;
      } else if (tokenDoc["expires_at"].IsString()) {
        if (auto parsed =
                parseUnixOrIsoTimestampSeconds(tokenDoc["expires_at"].GetString())) {
          tokenExpiration_ = *parsed;
        }
      }
    }

    tokenReceived_.store(true);
    isComplete_.store(true);
    if (!accessToken_.empty()) {
      fetchUserEmail();
    }
    if (email_.empty()) {
      email_ = importedIdc ? "idc-placeholder@awsapps.local" : "desktop-placeholder@awsapps.local";
    }

    OAuthAccount acc;
    acc.accessToken = accessToken_;
    acc.refreshToken = refreshToken_;
    acc.tokenExpiration = tokenExpiration_;
    acc.identifier = stableKiroAccountIdentifier(authMethod_, email_, clientId_,
                                                 profileArn_);
    acc.metadata["authMethod"] = authMethod_;
    acc.metadata["region"] = region_;
    if (!email_.empty()) acc.metadata["email"] = email_;
    if (!clientId_.empty()) acc.metadata["clientId"] = clientId_;
    if (!clientSecret_.empty()) acc.metadata["clientSecret"] = clientSecret_;
    if (!profileArn_.empty()) acc.metadata["profileArn"] = profileArn_;
    if (!startUrl_.empty()) acc.metadata["startUrl"] = startUrl_;
    provider_->addAccount(acc);
    return true;
  }

  void startDeviceFlow() {
    std::string registerUrl = KiroProvider::buildUrl("https://oidc.{{region}}.amazonaws.com/client/register", region_);

    rapidjson::Document reqDoc;
    reqDoc.SetObject();
    auto &alloc = reqDoc.GetAllocator();
    reqDoc.AddMember("clientName", "Kiro IDE", alloc);
    reqDoc.AddMember("clientType", "public", alloc);

    rapidjson::Value scopes(rapidjson::kArrayType);
    scopes.PushBack("codewhisperer:completions", alloc);
    scopes.PushBack("codewhisperer:analysis", alloc);
    scopes.PushBack("codewhisperer:conversations", alloc);
    scopes.PushBack("codewhisperer:transformations", alloc);
    scopes.PushBack("codewhisperer:taskassist", alloc);
    reqDoc.AddMember("scopes", scopes, alloc);

    rapidjson::Value grantTypes(rapidjson::kArrayType);
    grantTypes.PushBack("urn:ietf:params:oauth:grant-type:device_code", alloc);
    grantTypes.PushBack("refresh_token", alloc);
    reqDoc.AddMember("grantTypes", grantTypes, alloc);

    GCPHttpClient client("KiroIDE");
    client.setContentType("application/json");
    auto resp = client.post(registerUrl, jsonString(reqDoc));

    if (resp.code != 200) {
      setErrorAndComplete("Failed to register OAuth client: HTTP " + std::to_string(resp.code));
      return;
    }

    rapidjson::Document doc;
    doc.Parse(resp.body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
      setErrorAndComplete("Failed to parse client registration response");
      return;
    }

    clientId_ = jsonStringMember(doc, {"clientId"});
    clientSecret_ = jsonStringMember(doc, {"clientSecret"});
    if (clientId_.empty() || clientSecret_.empty()) {
      setErrorAndComplete("Missing clientId or clientSecret in registration response");
      return;
    }

    std::string deviceAuthUrl = KiroProvider::buildUrl("https://oidc.{{region}}.amazonaws.com/device_authorization", region_);

    rapidjson::Document deviceReq;
    deviceReq.SetObject();
    auto &deviceAlloc = deviceReq.GetAllocator();
    deviceReq.AddMember("clientId", rapidjson::Value(clientId_.c_str(), deviceAlloc), deviceAlloc);
    deviceReq.AddMember("clientSecret", rapidjson::Value(clientSecret_.c_str(), deviceAlloc), deviceAlloc);
    deviceReq.AddMember("startUrl", rapidjson::Value(startUrl_.c_str(), deviceAlloc), deviceAlloc);

    resp = client.post(deviceAuthUrl, jsonString(deviceReq));
    if (resp.code != 200) {
      setErrorAndComplete("Failed to start device authorization: HTTP " + std::to_string(resp.code));
      return;
    }

    doc.Parse(resp.body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
      setErrorAndComplete("Failed to parse device authorization response");
      return;
    }

    deviceCode_ = jsonStringMember(doc, {"deviceCode", "device_code"});
    userCode_ = jsonStringMember(doc, {"userCode", "user_code"});
    verificationUri_ = jsonStringMember(doc, {"verificationUri", "verification_uri"});
    verificationUriComplete_ = jsonStringMember(doc, {"verificationUriComplete", "verification_uri_complete"});
    if (doc.HasMember("expiresIn") && doc["expiresIn"].IsInt()) {
      expiresIn_ = doc["expiresIn"].GetInt();
    } else if (doc.HasMember("expires_in") && doc["expires_in"].IsInt()) {
      expiresIn_ = doc["expires_in"].GetInt();
    }
    if (doc.HasMember("interval") && doc["interval"].IsInt()) {
      interval_ = doc["interval"].GetInt();
    }

    if (deviceCode_.empty() || userCode_.empty() || verificationUri_.empty() || verificationUriComplete_.empty()) {
      setErrorAndComplete("Invalid device authorization response");
      return;
    }

    prompt_ = "Open this URL to authorize Kiro:\n" + verificationUriComplete_ + "\n\n"
              "Code: " + userCode_ + "\n\n"
              "Press Enter after you finish the browser step, or wait for polling to complete.";

    pollingThread_ = std::thread([this]() { pollForToken(); });
  }

  void pollForToken() {
    std::string tokenUrl = KiroProvider::buildUrl("https://oidc.{{region}}.amazonaws.com/token", region_);
    GCPHttpClient client("KiroIDE");
    client.setContentType("application/json");

    std::uint64_t startTime = nowMs();
    int interval = interval_ * 1000;
    const std::uint64_t timeoutMs = static_cast<std::uint64_t>(expiresIn_) * 1000 - 3000;

    while ((nowMs() - startTime) < timeoutMs && !stopPolling_.load()) {
      rapidjson::Document tokenReq;
      tokenReq.SetObject();
      auto &alloc = tokenReq.GetAllocator();
      tokenReq.AddMember("clientId", rapidjson::Value(clientId_.c_str(), alloc), alloc);
      tokenReq.AddMember("clientSecret", rapidjson::Value(clientSecret_.c_str(), alloc), alloc);
      tokenReq.AddMember("deviceCode", rapidjson::Value(deviceCode_.c_str(), alloc), alloc);
      tokenReq.AddMember("grantType", "urn:ietf:params:oauth:grant-type:device_code", alloc);

      auto resp = client.post(tokenUrl, jsonString(tokenReq));
      rapidjson::Document doc;
      doc.Parse(resp.body.c_str());

      if (!doc.HasParseError() && doc.IsObject()) {
        std::string error = jsonStringMember(doc, {"error"});
        if (!error.empty()) {
          if (error == "authorization_pending") {
          } else if (error == "slow_down") {
            interval += 5000;
          } else if (error == "expired_token") {
            setErrorAndComplete("Device code expired. Please try again.");
            return;
          } else if (error == "access_denied") {
            setErrorAndComplete("Authorization denied.");
            return;
          } else {
            setErrorAndComplete("Token polling failed: " + error);
            return;
          }
        } else {
          accessToken_ = jsonStringMember(doc, {"access_token", "accessToken"});
          refreshToken_ = jsonStringMember(doc, {"refresh_token", "refreshToken"});
          if (doc.HasMember("expiresIn") && doc["expiresIn"].IsInt()) {
            tokenExpiration_ = nowSeconds() + doc["expiresIn"].GetInt();
          } else if (doc.HasMember("expires_in") && doc["expires_in"].IsInt()) {
            tokenExpiration_ = nowSeconds() + doc["expires_in"].GetInt();
          }

          if (!accessToken_.empty() && !refreshToken_.empty()) {
            fetchUserEmail();
            tokenReceived_.store(true);
            isComplete_.store(true);
            prompt_.clear();
            state_ = State::Idle;
            return;
          }
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(interval));
    }

    if (!tokenReceived_.load()) {
      setErrorAndComplete("Authorization timed out");
    }
  }

  void fetchUserEmail() {
    GCPHttpClient client("KiroIDE");
    client.setBearerToken(accessToken_);
    client.addHeader("x-amzn-kiro-agent-mode", "vibe");

    std::string usageUrl = KiroProvider::buildUrl("https://q.{{region}}.amazonaws.com/getUsageLimits", region_);
    usageUrl += "?isEmailRequired=true&origin=AI_EDITOR&resourceType=AGENTIC_REQUEST";
    if (!profileArn_.empty()) {
      usageUrl += "&profileArn=" + profileArn_;
    }

    auto resp = client.get(usageUrl);
    if (resp.code == 200) {
      rapidjson::Document doc;
      doc.Parse(resp.body.c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("userInfo") && doc["userInfo"].IsObject()) {
        auto &ui = doc["userInfo"];
        if (ui.HasMember("email") && ui["email"].IsString()) {
          email_ = ui["email"].GetString();
        }
      }
    }
  }

  KiroProvider *provider_;
  std::string prompt_;
  std::string errorMessage_;

  State state_ = State::ChooseAuthMethod;

  std::string authMethod_;
  std::string region_ = kDefaultRegion;
  std::string startUrl_;
  std::string clientId_;
  std::string clientSecret_;
  std::string deviceCode_;
  std::string userCode_;
  std::string verificationUri_;
  std::string verificationUriComplete_;
  int expiresIn_ = 600;
  int interval_ = 5;

  std::string accessToken_;
  std::string refreshToken_;
  int64_t tokenExpiration_ = 0;
  std::string email_;
  std::string profileArn_;

  bool importRequested_ = false;
  std::atomic<bool> isComplete_{false};
  std::atomic<bool> tokenReceived_{false};
  std::atomic<bool> stopPolling_{false};
  std::thread pollingThread_;
};

std::unique_ptr<OAuthWizard> KiroProvider::beginConnectionWizard() {
  return std::make_unique<KiroOAuthWizard>(this);
}

} // namespace firmius::provider
