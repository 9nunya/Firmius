// =============================================================================
// WindsurfProvider — streaming, quota, and model-discovery transport.
//
// Wire protocol notes
// -------------------
// We talk gRPC over HTTP/2 to https://server.codeium.com directly. The proto
// schema is the one reverse-engineered by rsvedant/opencode-windsurf-auth and
// the official WINDSURF_API_SPEC.md from the same repo.
//
// Frame layout (request and response):
//   [1 byte] compression flag (0 == uncompressed)
//   [4 bytes] big-endian payload length
//   [N bytes] protobuf payload
//
// HTTP headers:
//   :method = POST
//   content-type = application/grpc
//   te = trailers
//   authorization = Bearer <api_key>          (cloud-side auth)
//   x-api-key = <api_key>                     (Codeium also accepts this)
//
// Tool calling
// ------------
// The cloud RawGetChatMessage proto is not fully reverse-engineered for tool
// fields. We follow the upstream plugin's strategy: when tools are requested,
// build a JSON-planning prompt and parse the model's structured reply into
// ToolCall / ToolCallChunk events. Real protobuf tool fields are surfaced via
// the optional `tools` proto member on GetChatMessageRequest (field 5) and we
// set a feature flag `kEnableProtoTools` for an opt-in v2 path.
// =============================================================================

#include "providers/WindsurfProvider.hpp"

#include "providers/WindsurfModels.hpp"
#include "utils/GCPHttpClient.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::provider {

namespace {

// Forward declare constants defined in WindsurfProvider.cpp's anon ns by
// re-defining here — these are file-static so they don't link.
constexpr const char *kCodeiumServer = "https://server.codeium.com";
constexpr const char *kInferenceServer = "https://inference.codeium.com";
constexpr const char *kIdeName = "windsurf";
constexpr const char *kIdeVersionFallback = "1.13.104";
constexpr const char *kExtensionVersionFallback = "1.36.4";
constexpr const char *kUserAgent = "firmius-windsurf/1.0 (linux; x86_64)";

// gRPC service paths to try in priority order. The first one that yields a
// gRPC status code 0 wins for the lifetime of the process.
struct ServicePath {
  const char *base;       // e.g. "https://server.codeium.com"
  const char *path;       // e.g. "/exa.api_server_pb.ApiServerService/..."
};
constexpr ServicePath kChatPaths[] = {
    {kInferenceServer,
     "/exa.api_server_pb.ApiServerService/GetStreamingExternalChatCompletions"},
    {kCodeiumServer,
     "/exa.language_server_pb.LanguageServerService/RawGetChatMessage"},
    {kCodeiumServer,
     "/exa.api_server_pb.ApiServerService/GetChatMessage"},
};

// Cached "this path works" — populated on first successful 200/grpc-status:0
// to skip probing on subsequent calls.
std::atomic<int> g_chatPathIndex{-1};

// ===========================================================================
// Protobuf primitives (varint + length-delim + frame)
// ===========================================================================

void writeVarint(std::string &out, std::uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
}

std::uint64_t readVarint(const std::uint8_t *buf, std::size_t len,
                         std::size_t &consumed) {
  std::uint64_t result = 0;
  int shift = 0;
  consumed = 0;
  while (consumed < len) {
    std::uint8_t b = buf[consumed++];
    result |= static_cast<std::uint64_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) return result;
    shift += 7;
    if (shift > 63) break;
  }
  return result;
}

void writeStringField(std::string &out, int fieldNum, std::string_view value) {
  std::uint64_t tag = (static_cast<std::uint64_t>(fieldNum) << 3) | 2;
  writeVarint(out, tag);
  writeVarint(out, value.size());
  out.append(value.data(), value.size());
}

void writeVarintField(std::string &out, int fieldNum, std::uint64_t value) {
  std::uint64_t tag = (static_cast<std::uint64_t>(fieldNum) << 3) | 0;
  writeVarint(out, tag);
  writeVarint(out, value);
}

void writeFixed64Field(std::string &out, int fieldNum, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  std::uint64_t tag = (static_cast<std::uint64_t>(fieldNum) << 3) | 1;
  writeVarint(out, tag);
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
  }
}

void writeMessageField(std::string &out, int fieldNum,
                       std::string_view payload) {
  std::uint64_t tag = (static_cast<std::uint64_t>(fieldNum) << 3) | 2;
  writeVarint(out, tag);
  writeVarint(out, payload.size());
  out.append(payload.data(), payload.size());
}

// 5-byte gRPC frame prefix.
void prependGrpcFrame(std::string &payload) {
  std::string framed;
  framed.reserve(payload.size() + 5);
  framed.push_back(0); // compression flag
  std::uint32_t len = static_cast<std::uint32_t>(payload.size());
  framed.push_back(static_cast<char>((len >> 24) & 0xFF));
  framed.push_back(static_cast<char>((len >> 16) & 0xFF));
  framed.push_back(static_cast<char>((len >> 8) & 0xFF));
  framed.push_back(static_cast<char>(len & 0xFF));
  framed.append(payload);
  payload = std::move(framed);
}

// ===========================================================================
// Wire encoders for Windsurf proto messages
// ===========================================================================

enum ChatSource : int {
  kSourceUnspecified = 0,
  kSourceUser = 1,
  kSourceSystem = 2,
  kSourceAssistant = 3,
  kSourceTool = 4,
};

std::string encodeTimestamp(std::int64_t epochSeconds) {
  std::string out;
  writeVarintField(out, 1, static_cast<std::uint64_t>(epochSeconds));
  return out;
}

std::string encodeChatIntent(std::string_view text) {
  std::string inner;
  writeStringField(inner, 1, text);  // generic.text
  std::string out;
  writeMessageField(out, 1, inner);  // intent.generic
  return out;
}

std::string encodeChatMessage(std::string_view content, int source,
                              std::string_view conversationId) {
  std::string out;
  std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

  // field 1: message_id
  writeStringField(out, 1, std::string(36, '0'));
  // field 2: source
  writeVarintField(out, 2, static_cast<std::uint64_t>(source));
  // field 3: timestamp
  std::string ts = encodeTimestamp(now);
  writeMessageField(out, 3, ts);
  // field 4: conversation_id
  writeStringField(out, 4, conversationId);
  // field 5: USER/SYSTEM/TOOL → intent (ChatMessageIntent), ASSISTANT → text
  if (source == kSourceAssistant) {
    writeStringField(out, 5, content);
  } else {
    std::string intent = encodeChatIntent(content);
    writeMessageField(out, 5, intent);
  }
  return out;
}

std::string encodeMetadata(std::string_view apiKey,
                           std::string_view ideVersion,
                           std::string_view extVersion,
                           std::string_view sessionId) {
  std::string out;
  // codeium_common_pb.Metadata, verified against:
  //   - dashboard Connect-RPC GetCascadeModelConfigs
  //   - language_server `--stdin_initial_metadata`
  //   - embedded protobuf tags in language_server_linux_x64
  writeStringField(out, 1, kIdeName);    // ide_name
  writeStringField(out, 2, extVersion);  // extension_version
  writeStringField(out, 3, apiKey);      // api_key
  writeStringField(out, 4, "en");        // locale
  writeStringField(out, 7, ideVersion);  // ide_version
  writeStringField(out, 12, kIdeName);
  if (!sessionId.empty()) {
    writeStringField(out, 10, sessionId); // session_id
  }
  return out;
}

std::string encodeRemoteApiChatMessage(std::string_view role,
                                        std::string_view content,
                                        std::string_view toolCallId = {}) {
  std::string out;
  writeStringField(out, 1, role);
  writeStringField(out, 2, content);
  if (!toolCallId.empty()) {
    writeStringField(out, 3, toolCallId);
  }
  return out;
}

std::string encodeRemoteModelOrAlias(int enumValue, std::string_view modelUid) {
  std::string out;
  if (enumValue > 0) {
    writeVarintField(out, 1, static_cast<std::uint64_t>(enumValue));
  }
  if (!modelUid.empty()) {
    writeStringField(out, 2, modelUid);
  }
  return out;
}

// ===========================================================================
// Response decoder
// ===========================================================================

struct ParsedField {
  int fieldNum = 0;
  int wireType = 0;
  std::uint64_t varint = 0;
  std::string_view bytes;
  std::size_t consumed = 0;
};

bool parseField(const std::uint8_t *data, std::size_t len, ParsedField &out) {
  if (len == 0) return false;
  std::size_t off = 0;
  std::size_t c;
  std::uint64_t tag = readVarint(data, len, c);
  if (c == 0) return false;
  off += c;
  out.fieldNum = static_cast<int>(tag >> 3);
  out.wireType = static_cast<int>(tag & 0x07);
  if (out.wireType == 0) {
    out.varint = readVarint(data + off, len - off, c);
    if (c == 0) return false;
    off += c;
  } else if (out.wireType == 2) {
    std::uint64_t l = readVarint(data + off, len - off, c);
    if (c == 0) return false;
    off += c;
    if (off + l > len) return false;
    out.bytes = std::string_view(reinterpret_cast<const char *>(data + off),
                                 static_cast<std::size_t>(l));
    off += static_cast<std::size_t>(l);
  } else if (out.wireType == 5) {
    if (off + 4 > len) return false;
    out.bytes = std::string_view(reinterpret_cast<const char *>(data + off), 4);
    off += 4;
  } else if (out.wireType == 1) {
    if (off + 8 > len) return false;
    out.bytes = std::string_view(reinterpret_cast<const char *>(data + off), 8);
    off += 8;
  } else {
    return false;
  }
  out.consumed = off;
  return true;
}

struct UnaryGrpcResult {
  int httpStatus = 0;
  int grpcStatus = -1;
  std::string grpcMessage;
  std::string body;
};

bool runGrpcUnary(const std::string &fullUrl, const std::string &apiKey,
                  const std::string &payload, UnaryGrpcResult &out,
                  std::string &outError, int timeoutSeconds = 30) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    outError = "curl init failed";
    return false;
  }

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "content-type: application/grpc");
  headers = curl_slist_append(headers, "te: trailers");
  headers = curl_slist_append(
      headers, ("authorization: Bearer " + apiKey).c_str());
  headers = curl_slist_append(headers, ("x-api-key: " + apiKey).c_str());

  auto headerCb = [](char *data, std::size_t size, std::size_t nmemb,
                     void *ud) -> std::size_t {
    auto *r = static_cast<UnaryGrpcResult *>(ud);
    std::size_t n = size * nmemb;
    std::string_view line(data, n);
    auto lower = [](std::string_view s) {
      std::string out(s);
      std::transform(out.begin(), out.end(), out.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      return out;
    };
    std::string l = lower(line);
    if (l.rfind("grpc-status:", 0) == 0) {
      try {
        r->grpcStatus = std::stoi(std::string(line.substr(12)));
      } catch (...) {
      }
    } else if (l.rfind("grpc-message:", 0) == 0) {
      r->grpcMessage = std::string(line.substr(13));
    }
    return n;
  };

  auto writeCb = [](char *data, std::size_t size, std::size_t nmemb,
                    void *ud) -> std::size_t {
    auto *r = static_cast<UnaryGrpcResult *>(ud);
    std::size_t n = size * nmemb;
    r->body.append(data, n);
    return n;
  };

  curl_easy_setopt(curl, CURLOPT_URL, fullUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                   static_cast<long>(CURL_HTTP_VERSION_2_0));
  curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  CURLcode rc = curl_easy_perform(curl);
  long http = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  out.httpStatus = static_cast<int>(http);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    outError = std::string("curl: ") + curl_easy_strerror(rc);
    return false;
  }
  if (out.httpStatus != 200) {
    outError = "HTTP " + std::to_string(out.httpStatus);
    return false;
  }
  if (out.grpcStatus != 0) {
    outError = "grpc-status " + std::to_string(out.grpcStatus) +
               (out.grpcMessage.empty() ? "" : ": " + out.grpcMessage);
    return false;
  }
  return true;
}

bool decodeGrpcFrames(const std::string &framed,
                      std::vector<std::string> &outMessages) {
  std::size_t off = 0;
  while (off + 5 <= framed.size()) {
    const auto *p = reinterpret_cast<const std::uint8_t *>(framed.data() + off);
    std::uint8_t compressed = p[0];
    std::uint32_t flen = (static_cast<std::uint32_t>(p[1]) << 24) |
                         (static_cast<std::uint32_t>(p[2]) << 16) |
                         (static_cast<std::uint32_t>(p[3]) << 8) |
                         static_cast<std::uint32_t>(p[4]);
    off += 5;
    if (off + flen > framed.size()) {
      return false;
    }
    if (compressed == 0) {
      outMessages.emplace_back(framed.data() + off, flen);
    }
    off += flen;
  }
  return true;
}

struct GrpcDiscoveredModel {
  int enumValue = 0;
  std::string canonicalId;
};

std::vector<GrpcDiscoveredModel>
discoverModelsViaGrpc(const firmius::shared::OAuthAccount &acc) {
  std::vector<GrpcDiscoveredModel> models;

  std::string sessionId =
      acc.metadata.count("sessionId") ? acc.metadata.at("sessionId") : "";
  if (sessionId.empty()) {
    sessionId = std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
  }
  std::string metadata =
      encodeMetadata(acc.accessToken, kIdeVersionFallback,
                     kExtensionVersionFallback, sessionId);
  std::string req;
  // GetModelStatusesRequest field 1 = Metadata
  writeMessageField(req, 1, metadata);
  prependGrpcFrame(req);

  UnaryGrpcResult unary;
  std::string err;
  const std::string url = std::string(kCodeiumServer) +
                          "/exa.language_server_pb.LanguageServerService/"
                          "GetModelStatuses";
  if (!runGrpcUnary(url, acc.accessToken, req, unary, err, 30)) {
    return models;
  }

  std::vector<std::string> payloads;
  if (!decodeGrpcFrames(unary.body, payloads)) {
    return models;
  }

  for (const auto &payload : payloads) {
    std::size_t off = 0;
    while (off < payload.size()) {
      ParsedField f;
      if (!parseField(reinterpret_cast<const std::uint8_t *>(payload.data() + off),
                      payload.size() - off, f)) {
        break;
      }
      off += f.consumed;

      // GetModelStatusesResponse field 1 = repeated ModelStatusInfo
      if (f.fieldNum != 1 || f.wireType != 2) {
        continue;
      }

      GrpcDiscoveredModel cm;

      std::size_t infoOff = 0;
      while (infoOff < f.bytes.size()) {
        ParsedField mf;
        if (!parseField(
                reinterpret_cast<const std::uint8_t *>(f.bytes.data() + infoOff),
                f.bytes.size() - infoOff, mf)) {
          break;
        }
        infoOff += mf.consumed;

        // ModelStatusInfo field 1 = model enum
        // ModelStatusInfo field 4 = model_uid
        // ModelStatusInfo field 8 = enabled? (best-effort, may not exist)
        if (mf.fieldNum == 1 && mf.wireType == 0) {
          cm.enumValue = static_cast<int>(mf.varint);
        } else if (mf.fieldNum == 4 && mf.wireType == 2) {
          cm.canonicalId.assign(mf.bytes.data(), mf.bytes.size());
        }
      }

      if (cm.enumValue <= 0) {
        continue;
      }
      if (cm.canonicalId.empty()) {
        cm.canonicalId = windsurf::canonicalIdFromEnum(cm.enumValue);
      }
      if (cm.canonicalId.empty()) {
        cm.canonicalId = "model-" + std::to_string(cm.enumValue);
      }
      models.push_back(std::move(cm));
    }
  }

  return models;
}

bool isSessionToken(std::string_view token) {
  return token.rfind("devin-session-token$", 0) == 0;
}

bool looksLikeJwtToken(std::string_view token) {
  return token.find('.') != std::string_view::npos &&
         token.find('.', token.find('.') + 1) != std::string_view::npos;
}

std::string makeSessionToken(const firmius::shared::OAuthAccount &acc) {
  if (isSessionToken(acc.accessToken)) {
    return acc.accessToken;
  }
  if (looksLikeJwtToken(acc.refreshToken)) {
    return "devin-session-token$" + acc.refreshToken;
  }
  return {};
}

std::string extractFirebaseToken(const firmius::shared::OAuthAccount &acc) {
  if (looksLikeJwtToken(acc.refreshToken)) {
    return acc.refreshToken;
  }
  if (isSessionToken(acc.accessToken)) {
    return acc.accessToken.substr(std::string("devin-session-token$").size());
  }
  return {};
}

// Calls ApiServerService.AssignModel against the same host the Windsurf LSP
// targets (--api_server_url defaults to https://server.self-serve.windsurf.com)
// using Connect-Unary (HTTP/1.1, application/proto, bare protobuf body — no
// gRPC framing). Returns the assignment_jwt that the Cascade router requires
// for routing chat models, or empty on failure.
std::string assignModelForChat(const std::string &cloudApiKey,
                               const std::string &sessionToken,
                               std::string_view metadata,
                               std::string_view modelRouterUid,
                               std::string_view cascadeId,
                               std::string_view /*chatMessagePrompt*/,
                               std::string &outErr) {
  // AssignModelRequest:
  //   1: metadata, 2: model_router_uid, 3: cascade_id.
  // The older chat_message_prompt field guess was producing upstream
  // invalid_argument/internal errors across every host we probed; keep the
  // minimal routing tuple until the full request shape is confirmed.
  std::string body;
  writeMessageField(body, 1, metadata);
  writeStringField(body, 2, modelRouterUid);
  writeStringField(body, 3, cascadeId);

  const bool dbg = std::getenv("FIRMIUS_WINDSURF_DEBUG") != nullptr;
  if (dbg) {
    std::fprintf(stderr,
                 "[windsurf-remote] AssignModel POST body=%zu uid=%.*s\n",
                 body.size(), static_cast<int>(modelRouterUid.size()),
                 modelRouterUid.data());
  }
  // The Windsurf LSP itself talks to one of these hosts via connectrpc/Go.
  // Try each — first 200 response wins.
  static constexpr const char *kAssignHosts[] = {
      "https://inference.codeium.com",
      "https://server.codeium.com",
      "https://server.self-serve.windsurf.com",
      "https://windsurf.com/_backend",
  };
  std::string respBody;
  long respCode = 0;
  for (const char *host : kAssignHosts) {
    firmius::utils::GCPHttpClient client(kUserAgent);
    client.setContentType("application/proto");
    client.addHeader("connect-protocol-version", "1");
    client.addHeader("accept", "*/*");

    const bool backendHost =
        std::string_view(host) == "https://windsurf.com/_backend";
    if (backendHost) {
      if (sessionToken.empty()) {
        continue;
      }
      client.addHeader("x-devin-session-token", sessionToken);
    } else {
      client.addHeader("authorization", "Bearer " + cloudApiKey);
      client.addHeader("x-api-key", cloudApiKey);
      if (!sessionToken.empty()) {
        client.addHeader("x-devin-session-token", sessionToken);
      }
    }

    auto resp = client.post(
        std::string(host) +
            "/exa.api_server_pb.ApiServerService/AssignModel",
        body, 30);
    if (dbg) {
      std::fprintf(stderr,
                   "[windsurf-remote] AssignModel %s <- code=%ld len=%zu body=%.*s\n",
                   host, resp.code, resp.body.size(),
                   static_cast<int>(std::min<std::size_t>(resp.body.size(), 256)),
                   resp.body.data());
    }
    if (resp.code == 200) {
      respBody = std::move(resp.body);
      respCode = resp.code;
      break;
    }
    respBody = std::move(resp.body);
    respCode = resp.code;
  }
  if (respCode != 200) {
    outErr = "AssignModel HTTP " + std::to_string(respCode) + ": " +
             respBody.substr(0, 200);
    return {};
  }
  const std::string &respBytes = respBody;
  // AssignModelResponse { 1: assignment (ModelAssignment) }
  // ModelAssignment     { 1: assignment_jwt, 2: model_uid, 3: harness_uids }
  std::size_t off = 0;
  while (off < respBytes.size()) {
    ParsedField f;
    if (!parseField(reinterpret_cast<const std::uint8_t *>(respBytes.data() +
                                                           off),
                    respBytes.size() - off, f))
      break;
    off += f.consumed;
    if (f.fieldNum == 1 && f.wireType == 2) {
      std::size_t io = 0;
      while (io < f.bytes.size()) {
        ParsedField ff;
        if (!parseField(reinterpret_cast<const std::uint8_t *>(f.bytes.data() +
                                                               io),
                        f.bytes.size() - io, ff))
          break;
        io += ff.consumed;
        if (ff.fieldNum == 1 && ff.wireType == 2) {
          return std::string(ff.bytes);
        }
      }
    }
  }
  outErr = "AssignModel: no assignment_jwt in response";
  return {};
}

// Pull `text` (field 5) out of a RawChatMessage payload.
std::string extractTextFromRawChatMessage(std::string_view buf) {
  std::size_t off = 0;
  while (off < buf.size()) {
    ParsedField f;
    if (!parseField(reinterpret_cast<const std::uint8_t *>(buf.data() + off),
                    buf.size() - off, f)) {
      break;
    }
    off += f.consumed;
    if (f.fieldNum == 5 && f.wireType == 2) {
      return std::string(f.bytes);
    }
  }
  return {};
}

// CompletionDelta (codeium_common_pb): field 1 = delta_text.
std::string extractTextFromCompletionDelta(std::string_view buf) {
  std::size_t off = 0;
  std::string text;
  while (off < buf.size()) {
    ParsedField f;
    if (!parseField(reinterpret_cast<const std::uint8_t *>(buf.data() + off),
                    buf.size() - off, f)) break;
    off += f.consumed;
    if (f.fieldNum == 1 && f.wireType == 2) {
      text.append(f.bytes.data(), f.bytes.size());
    }
  }
  return text;
}

// CompletionDeltaMap: field 1 (repeated) = DeltasEntry { 1: int32 key,
// 2: CompletionDelta value }.
std::string extractTextFromCompletionDeltaMap(std::string_view buf) {
  std::size_t off = 0;
  std::string text;
  while (off < buf.size()) {
    ParsedField f;
    if (!parseField(reinterpret_cast<const std::uint8_t *>(buf.data() + off),
                    buf.size() - off, f)) break;
    off += f.consumed;
    if (f.fieldNum == 1 && f.wireType == 2) {
      // DeltasEntry — find inner field 2 (the CompletionDelta value).
      std::size_t eo = 0;
      while (eo < f.bytes.size()) {
        ParsedField ef;
        if (!parseField(reinterpret_cast<const std::uint8_t *>(
                            f.bytes.data() + eo),
                        f.bytes.size() - eo, ef)) break;
        eo += ef.consumed;
        if (ef.fieldNum == 2 && ef.wireType == 2) {
          text += extractTextFromCompletionDelta(ef.bytes);
        }
      }
    }
  }
  return text;
}

// Top-level streaming-response decoder. Handles three shapes:
//   - RawGetChatMessageResponse (path[1]): field 1 = delta_message
//     (RawChatMessage) — handled by extractTextFromRawChatMessage.
//   - GetStreamingExternalChatCompletionsResponse (path[0]): field 1 =
//     delta_map (CompletionDeltaMap).
//   - GetChatMessageResponse (path[2]): field 3 = delta_text (string).
std::string extractTextFromResponseFrame(std::string_view buf) {
  std::size_t off = 0;
  std::string text;
  while (off < buf.size()) {
    ParsedField f;
    if (!parseField(reinterpret_cast<const std::uint8_t *>(buf.data() + off),
                    buf.size() - off, f)) {
      break;
    }
    off += f.consumed;
    if (f.fieldNum == 1 && f.wireType == 2) {
      // Try the RawChatMessage shape first; if it yields nothing, fall back
      // to the CompletionDeltaMap shape (path[0]).
      auto t = extractTextFromRawChatMessage(f.bytes);
      if (t.empty()) t = extractTextFromCompletionDeltaMap(f.bytes);
      if (!t.empty()) text += t;
    } else if (f.fieldNum == 3 && f.wireType == 2) {
      // ApiServerService.GetChatMessageResponse.delta_text.
      text.append(f.bytes.data(), f.bytes.size());
    }
  }
  return text;
}

// ===========================================================================
// gRPC streaming over HTTP/2 (libcurl)
// ===========================================================================

struct StreamCtx {
  std::string buffer;
  std::size_t consumed = 0;
  std::function<void(std::string_view textDelta)> onText;
  std::atomic<bool> *abort = nullptr;
  // gRPC trailers
  int grpcStatus = -1;
  std::string grpcMessage;
};

std::size_t streamWriteCb(char *data, std::size_t size, std::size_t nmemb,
                          void *userdata) {
  auto *ctx = static_cast<StreamCtx *>(userdata);
  if (ctx->abort && ctx->abort->load()) return 0;
  std::size_t total = size * nmemb;
  ctx->buffer.append(data, total);

  // Drain whole frames.
  while (ctx->buffer.size() - ctx->consumed >= 5) {
    const auto *p = reinterpret_cast<const std::uint8_t *>(
        ctx->buffer.data() + ctx->consumed);
    std::uint8_t compressed = p[0];
    std::uint32_t flen = (static_cast<std::uint32_t>(p[1]) << 24) |
                         (static_cast<std::uint32_t>(p[2]) << 16) |
                         (static_cast<std::uint32_t>(p[3]) << 8) |
                         static_cast<std::uint32_t>(p[4]);
    if (ctx->buffer.size() - ctx->consumed - 5 < flen) break;
    if (compressed == 0) {
      std::string_view payload(ctx->buffer.data() + ctx->consumed + 5, flen);
      auto text = extractTextFromResponseFrame(payload);
      if (!text.empty() && ctx->onText) ctx->onText(text);
    }
    ctx->consumed += 5 + flen;
  }
  // Compact buffer occasionally.
  if (ctx->consumed > 1 << 20) {
    ctx->buffer.erase(0, ctx->consumed);
    ctx->consumed = 0;
  }
  return total;
}

std::size_t streamHeaderCb(char *data, std::size_t size, std::size_t nmemb,
                           void *userdata) {
  auto *ctx = static_cast<StreamCtx *>(userdata);
  std::size_t n = size * nmemb;
  std::string_view line(data, n);
  // gRPC trailers arrive as headers in HTTP/2.
  auto lower = [](std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
  };
  std::string l = lower(line);
  if (l.rfind("grpc-status:", 0) == 0) {
    try {
      ctx->grpcStatus = std::stoi(std::string(line.substr(12)));
    } catch (...) {
    }
  } else if (l.rfind("grpc-message:", 0) == 0) {
    ctx->grpcMessage = std::string(line.substr(13));
  }
  return n;
}

// Returns true if the stream completed with grpc-status:0.
bool runGrpcStream(const std::string &fullUrl, const std::string &apiKey,
                   const std::string &payload, StreamCtx &ctx,
                   std::string &outError, int timeoutSeconds = 300) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    outError = "curl init failed";
    return false;
  }
  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "content-type: application/grpc");
  headers = curl_slist_append(headers, "te: trailers");
  headers = curl_slist_append(
      headers, ("authorization: Bearer " + apiKey).c_str());
  headers = curl_slist_append(headers, ("x-api-key: " + apiKey).c_str());

  curl_easy_setopt(curl, CURLOPT_URL, fullUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                   static_cast<long>(CURL_HTTP_VERSION_2_0));
  curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, streamHeaderCb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                   static_cast<long>(timeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  CURLcode rc = curl_easy_perform(curl);
  long http = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    outError = std::string("curl: ") + curl_easy_strerror(rc);
    return false;
  }
  if (http != 200) {
    outError = "HTTP " + std::to_string(http);
    return false;
  }
  if (ctx.grpcStatus != 0) {
    outError = "grpc-status " + std::to_string(ctx.grpcStatus) +
               (ctx.grpcMessage.empty() ? "" : ": " + ctx.grpcMessage);
    return false;
  }
  return true;
}

// ===========================================================================
// Tool-planning prompt (mirrors upstream JSON contract)
// ===========================================================================

std::string buildToolPlanningSystem(
    const std::vector<ToolDefinition> &tools) {
  std::ostringstream s;
  s << "You are a tool-calling agent. Available tools:\n";
  for (const auto &t : tools) {
    s << "  - " << t.name << ": " << t.description << "\n    schema: "
      << t.inputSchema << "\n";
  }
  s << R"(

STRICT OUTPUT FORMAT:
Output ONLY one JSON object. No prose, no <tool_call> tags.

To call tools (ONE OR MORE in parallel allowed):
  {"action":"tool_call","tool_calls":[{"name":"<name>","arguments":{...}}, ...]}

To produce a final answer:
  {"action":"final","content":"<text>"}
)";
  return s.str();
}

struct ParsedToolPlan {
  bool isToolCall = false;
  std::string finalContent;
  struct Call {
    std::string name;
    std::string argsJson;
  };
  std::vector<Call> calls;
};

ParsedToolPlan parseToolPlan(const std::string &raw) {
  ParsedToolPlan plan;
  auto first = raw.find('{');
  auto last = raw.rfind('}');
  if (first == std::string::npos || last == std::string::npos || last <= first) {
    plan.finalContent = raw;
    return plan;
  }
  std::string js = raw.substr(first, last - first + 1);
  rapidjson::Document doc;
  doc.Parse(js.c_str());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("action") ||
      !doc["action"].IsString()) {
    plan.finalContent = raw;
    return plan;
  }
  std::string action = doc["action"].GetString();
  if (action == "final" && doc.HasMember("content") &&
      doc["content"].IsString()) {
    plan.finalContent = doc["content"].GetString();
    return plan;
  }
  if (action == "tool_call" && doc.HasMember("tool_calls") &&
      doc["tool_calls"].IsArray()) {
    plan.isToolCall = true;
    for (const auto &tc : doc["tool_calls"].GetArray()) {
      if (!tc.IsObject() || !tc.HasMember("name") || !tc["name"].IsString()) {
        continue;
      }
      ParsedToolPlan::Call c;
      c.name = tc["name"].GetString();
      if (tc.HasMember("arguments")) {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        tc["arguments"].Accept(w);
        c.argsJson = std::string(buf.GetString(), buf.GetSize());
      } else {
        c.argsJson = "{}";
      }
      plan.calls.push_back(std::move(c));
    }
    return plan;
  }
  plan.finalContent = raw;
  return plan;
}

// ===========================================================================
// REST helpers (quota / model discovery)
// ===========================================================================

bool postJson(const std::string &url, const std::string &apiKey,
              const std::string &body, std::string &outResp,
              int timeoutSeconds = 30) {
  firmius::utils::GCPHttpClient client(kUserAgent);
  client.setContentType("application/json");
  client.setBearerToken(apiKey);
  client.addHeader("x-api-key", apiKey);
  auto resp = client.post(url, body, timeoutSeconds);
  outResp = resp.body;
  return resp.code == 200;
}

// =============================================================================
// Connect-RPC unary helper for windsurf.com/_backend (the dashboard backend).
//
// Wire shape (verified live against an authenticated account, see
// docs/REVERSE_ENGINEERING_NOTES below):
//   POST https://windsurf.com/_backend/<service>/<method>
//   content-type: application/proto
//   connect-protocol-version: 1
//   x-devin-session-token: devin-session-token$<jwt>
//   body: bare protobuf message (NO gRPC framing — Connect-Unary, not gRPC).
//
// The auth token also goes in the request body when the proto defines an
// `auth_token` scalar field (e.g. SeatManagementService.GetCurrentUser /
// GetPlanStatus). For methods that take a `Metadata` message instead (e.g.
// ApiServerService.GetCascadeModelConfigs) we put the same token into
// `Metadata.api_key` (field 3).
// =============================================================================

constexpr const char *kBackendBase = "https://windsurf.com/_backend";

bool runConnectUnary(const std::string &path,
                     const std::string &sessionToken,
                     const std::string &body,
                     std::string &outResponse,
                     std::string &outError,
                     int timeoutSeconds = 8) {
  const bool debug = std::getenv("FIRMIUS_WINDSURF_DEBUG") != nullptr;
  if (debug) {
    std::fprintf(stderr,
                 "[windsurf] connect-rpc POST %s%s body=%zu tok_prefix=%.20s\n",
                 kBackendBase, path.c_str(), body.size(), sessionToken.c_str());
  }
  firmius::utils::GCPHttpClient client(kUserAgent);
  client.setContentType("application/proto");
  client.addHeader("connect-protocol-version", "1");
  client.addHeader("x-devin-session-token", sessionToken);
  client.addHeader("accept", "*/*");
  auto resp = client.post(kBackendBase + path, body, timeoutSeconds);
  if (debug) {
    std::fprintf(stderr,
                 "[windsurf] connect-rpc <- code=%ld body_len=%zu err=%.200s\n",
                 resp.code, resp.body.size(), resp.error.c_str());
  }
  if (resp.code != 200) {
    outError = "HTTP " + std::to_string(resp.code) +
               (resp.body.empty() ? "" : ": " + resp.body.substr(0, 200));
    return false;
  }
  outResponse = std::move(resp.body);
  return true;
}

// Build the codeium_common_pb.Metadata message used by ApiServerService
// methods. Field tags taken verbatim from the dashboard JS bundle.
std::string encodeBackendMetadata(const std::string &sessionToken,
                                  const std::string &sessionId) {
  std::string out;
  // Required-ish fields
  writeStringField(out, 1, "windsurf");           // ide_name
  writeStringField(out, 2, kExtensionVersionFallback); // extension_version
  writeStringField(out, 3, sessionToken);         // api_key (we put the
                                                  // session token here; the
                                                  // backend treats it as the
                                                  // bearer credential)
  writeStringField(out, 4, "en");                 // locale
  writeStringField(out, 7, kIdeVersionFallback);  // ide_version
  writeStringField(out, 10, sessionId);           // session_id
  return out;
}

} // namespace

// ===========================================================================
// stream() — main streaming entry point
// ===========================================================================

void WindsurfProvider::stream(
    const firmius::shared::AgentHistory &history,
    const ProviderOptions &opts,
    std::function<void(const firmius::shared::StreamEvent &)> onEvent) {

  auto accOpt = getAvailableAccount(opts.modelId);
  if (!accOpt) {
    onEvent(firmius::shared::StreamError{
        "No Windsurf account configured. Run /connect windsurf to log in.",
        401, ""});
    onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Error});
    return;
  }
  const auto &acc = *accOpt;
  std::string sessionToken = makeSessionToken(acc);
  std::string apiKey = extractFirebaseToken(acc);
  if (apiKey.empty()) {
    // Last-resort fallback for older accounts that persisted only the
    // register_user api_key. The direct cloud path appears to prefer the raw
    // Firebase ID token, but keeping this fallback preserves some chance of
    // compatibility on legacy accounts.
    apiKey = acc.accessToken;
  }

  const bool debug = std::getenv("FIRMIUS_WINDSURF_DEBUG") != nullptr;
  if (debug) {
    std::fprintf(stderr,
                 "[windsurf-remote] auth session=%d cloud_key_prefix=%.24s\n",
                 sessionToken.empty() ? 0 : 1, apiKey.c_str());
  }
  auto resolved = windsurf::resolveModel(opts.modelId);
  // The static `resolveModel` table only knows the historical Windsurf
  // catalog. For models that came in via the live Connect-RPC discovery
  // (e.g. `claude-opus-4-7-medium`, `gpt-5-5-low`, `swe-1-6`) we synthesize
  // a ResolvedModel from the dynamic cache so the stream call still works.
  if (!resolved) {
    std::lock_guard<std::recursive_mutex> lock(modelMutex_);
    for (const auto &cm : models_) {
      if (cm.canonicalId == opts.modelId) {
        windsurf::ResolvedModel r;
        r.canonicalId = cm.canonicalId;
        r.enumValue = cm.enumValue; // 0 if server didn't ship one — server
                                    // dispatches by wireModelName in that case
        r.wireModelName = cm.canonicalId;
        resolved = r;
        break;
      }
    }
  }
  if (!resolved) {
    onEvent(firmius::shared::StreamError{
        "Unknown Windsurf model: " + opts.modelId, 400, ""});
    onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Error});
    return;
  }

  if (debug) {
    std::fprintf(stderr,
                 "[windsurf-remote] resolved model uid=%s enum=%d\n",
                 resolved->wireModelName.c_str(), resolved->enumValue);
  }
  // Local-LSP fast path (preferred).
  //
  // We talk Connect-RPC to a running Windsurf language_server on
  // 127.0.0.1:<--server_port>. If a local LSP is reachable + authed, this
  // streams full Cascade responses (text + tool calls + cached_input_tokens)
  // and naturally preserves prompt cache hits across model switches via the
  // persisted threadId → cascade_id map.
  //
  // Force the remote-direct path with FIRMIUS_WINDSURF_REMOTE=1 (legacy /
  // headless), but note that path is currently broken on the latest server
  // and only useful for development.
  const char *forceRemote = std::getenv("FIRMIUS_WINDSURF_REMOTE");
  if (!forceRemote || forceRemote[0] == '0') {
    // Bridge: the static catalog's wireModelName is the proto enum name
    // (e.g. "CLAUDE_4_5_SONNET"), but the LSP only knows the live
    // model_uids returned by GetCascadeModelConfigs (e.g. "MODEL_PRIVATE_2"
    // for premium models, or slug-form "claude-opus-4-7-medium" for the
    // open ones). When we have a discovery entry with the same enum value,
    // prefer its canonicalId — that's the actual UID the LSP will accept.
    std::string lspUid = resolved->wireModelName;
    if (resolved->enumValue > 0) {
      std::lock_guard<std::recursive_mutex> lock(modelMutex_);
      for (const auto &cm : models_) {
        if (cm.fromDiscovery && cm.enumValue == resolved->enumValue &&
            !cm.canonicalId.empty()) {
          lspUid = cm.canonicalId;
          break;
        }
      }
    }
    if (streamChatLocal(acc, lspUid, resolved->enumValue, history, opts,
                        std::move(onEvent))) {
      return;
    }
    // streamChatLocal already emitted StreamError + StreamDone on failure.
    return;
  }

  // ---- Build the proto request ----
  std::string sessionId =
      acc.metadata.count("sessionId") ? acc.metadata.at("sessionId") : "";
  if (sessionId.empty()) sessionId = std::to_string(std::rand());
  std::string ideVersion = kIdeVersionFallback;
  std::string extVersion = kExtensionVersionFallback;
  std::string conversationId = std::to_string(
      std::chrono::system_clock::now().time_since_epoch().count());

  std::string metadata =
      encodeMetadata(apiKey, ideVersion, extVersion, sessionId);

  std::string req;
  writeMessageField(req, 1, metadata);

  // Effective system prompt — when tools are present, we inject a JSON
  // tool-planning preamble.
  std::string systemPrompt;
  bool toolsActive = !opts.tools.empty();
  if (toolsActive) systemPrompt = buildToolPlanningSystem(opts.tools);

  // Walk history → repeated chat_messages (field 2) + system_prompt_override
  // (field 3).
  std::string latestUserText;
  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == firmius::shared::Role::Error) continue;
      if (msg.role == firmius::shared::Role::System) {
        for (const auto &p : msg.content) {
          if (auto *t = std::get_if<firmius::shared::TextContent>(&p)) {
            if (!systemPrompt.empty()) systemPrompt += "\n\n";
            systemPrompt += t->text;
          }
        }
        continue;
      }
      int source = (msg.role == firmius::shared::Role::User)
                       ? kSourceUser
                       : (msg.role == firmius::shared::Role::Assistant)
                             ? kSourceAssistant
                       : (msg.role == firmius::shared::Role::ToolResult)
                             ? kSourceTool
                             : kSourceUser;
      std::string text;
      for (const auto &p : msg.content) {
        if (auto *t = std::get_if<firmius::shared::TextContent>(&p)) {
          text += t->text;
        } else if (auto *thk =
                       std::get_if<firmius::shared::ThinkingContent>(&p)) {
          // We don't replay thinking deltas back to the server — they aren't
          // first-class in the wire format and confuse the model.
          (void)thk;
        } else if (auto *tcc =
                       std::get_if<firmius::shared::ToolCallContent>(&p)) {
          text += "\n[ASSISTANT TOOL_CALL " + tcc->name + " " + tcc->args +
                  "]";
        } else if (auto *tr =
                       std::get_if<firmius::shared::ToolResultContent>(&p)) {
          text += "\n[TOOL_RESULT id=" + tr->toolCallId + ": " + tr->result +
                  "]";
        }
      }
      if (text.empty()) continue;
      if (msg.role == firmius::shared::Role::User) {
        latestUserText = text;
      }
      auto chatMsg = encodeChatMessage(text, source, conversationId);
      writeMessageField(req, 2, chatMsg);
    }
  }

  if (!systemPrompt.empty()) {
    writeStringField(req, 3, systemPrompt);
  }
  if (resolved->enumValue > 0) {
    std::string modelMsg;
    writeVarintField(modelMsg, 1,
                     static_cast<std::uint64_t>(resolved->enumValue));
    writeMessageField(req, 4, modelMsg);
  }
  writeStringField(req, 5, resolved->wireModelName);

  prependGrpcFrame(req);

  // ApiServerService.GetChatMessage has a different request type than
  // LanguageServerService.RawGetChatMessage. Build a dedicated cloud request
  // for path[2] instead of reusing the raw/local one.
  // Build a per-request UUIDish id for prompt_id tracking. The cloud router
  // uses this for de-duplication and quota attribution; it appears to silently
  // reject requests where it is missing.
  auto makeRequestId = [&]() {
    static std::atomic<std::uint64_t> counter{0};
    auto epoch = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
    std::ostringstream os;
    os << "firmius-" << ms << "-" << counter.fetch_add(1);
    return os.str();
  };
  const std::string requestId = makeRequestId();

  // ApiServerService.GetChatMessageRequest. Verified field numbers against
  // the embedded FileDescriptorProto in language_server_linux_x64.
  // Strategy: send the conversation via repeated ChatMessagePrompt only
  // (legacy `prompt` scalar omitted), and route by EITHER internal_chat_model
  // (enum, for catalog-known ids) OR chat_model_uid (string, for dynamically
  // discovered models) — never both, since the server treats the dual route
  // as an arena/eval request and bounces it.
  std::string apiReq;
  writeMessageField(apiReq, 1, metadata);
  std::string apiPrompt = latestUserText;
  if (!systemPrompt.empty()) {
    apiPrompt = systemPrompt + "\n\n" + apiPrompt;
  }
  std::string promptMsg;
  writeStringField(promptMsg, 1, requestId + "-u0"); // message_id
  writeVarintField(promptMsg, 2, kSourceUser);       // source
  writeStringField(promptMsg, 3, apiPrompt);         // prompt
  writeVarintField(promptMsg, 5, 1);                 // safe_for_code_telemetry
  writeMessageField(apiReq, 3, promptMsg);
  if (resolved->enumValue > 0) {
    writeVarintField(apiReq, 5, 1); // use_internal_chat_model = true
    writeVarintField(apiReq, 6,
                     static_cast<std::uint64_t>(resolved->enumValue));
  } else {
    // Dynamic model — server routes by uid; do NOT also set the enum.
    writeStringField(apiReq, 21, resolved->wireModelName); // chat_model_uid
  }
  writeVarintField(apiReq, 7, 5); // CHAT_MESSAGE_REQUEST_TYPE_CASCADE
  std::string cfg;
  writeVarintField(cfg, 2, 8192);                                   // max_tokens
  writeFixed64Field(cfg, 5, static_cast<double>(opts.temperature)); // temperature
  writeMessageField(apiReq, 8, cfg);
  // cascade_id (field 16) is required by the cloud Cascade router. We allocate
  // a stable per-conversation ID so retries land on the same cascade.
  std::string cascadeId =
      "firmius-cascade-" + std::to_string(std::chrono::duration_cast<
                                              std::chrono::milliseconds>(
                                              std::chrono::system_clock::now()
                                                  .time_since_epoch())
                                              .count());
  writeStringField(apiReq, 16, cascadeId);
  writeStringField(apiReq, 17, requestId);          // prompt_id
  writeVarintField(apiReq, 18, 2);                  // PROVIDER_SOURCE_CHAT
  writeVarintField(apiReq, 19, 30);                 // LANGUAGE_PLAINTEXT

  // Mint a model_assignment_jwt by calling AssignModel first. The cloud
  // rejects Cascade chat requests without it ("internal error" with a
  // server-side trace ID).
  {
    const bool dbg = std::getenv("FIRMIUS_WINDSURF_DEBUG") != nullptr;
    std::string assignErr;
    std::string jwt =
        assignModelForChat(apiKey, sessionToken, metadata,
                           resolved->wireModelName, cascadeId, promptMsg,
                           assignErr);
    if (!jwt.empty()) {
      writeStringField(apiReq, 26, jwt); // model_assignment_jwt
      if (dbg) {
        std::fprintf(stderr,
                     "[windsurf-remote] AssignModel ok jwt_len=%zu\n",
                     jwt.size());
      }
    } else if (dbg) {
      std::fprintf(stderr, "[windsurf-remote] AssignModel failed: %s\n",
                   assignErr.c_str());
    }
  }
  prependGrpcFrame(apiReq);

  // Spec-shaped direct cloud request for ApiServerService.GetChatMessage.
  // Remote request shape is simpler than the LS/local proto:
  //   1 metadata, 2 cascade_id, 3 model_or_alias, 4 repeated chat messages.
  std::string remoteApiReq;
  writeMessageField(remoteApiReq, 1, metadata);
  writeStringField(remoteApiReq, 2, cascadeId);
  std::string remoteModel =
      encodeRemoteModelOrAlias(resolved->enumValue, resolved->wireModelName);
  if (!remoteModel.empty()) {
    writeMessageField(remoteApiReq, 3, remoteModel);
  }
  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == firmius::shared::Role::Error) continue;
      std::string text;
      std::string toolCallId;
      for (const auto &p : msg.content) {
        if (auto *t = std::get_if<firmius::shared::TextContent>(&p)) {
          text += t->text;
        } else if (auto *tr =
                       std::get_if<firmius::shared::ToolResultContent>(&p)) {
          if (!text.empty()) text += "\n";
          text += tr->result;
          if (toolCallId.empty()) toolCallId = tr->toolCallId;
        }
      }
      if (text.empty()) continue;
      std::string role = "user";
      switch (msg.role) {
      case firmius::shared::Role::User: role = "user"; break;
      case firmius::shared::Role::Assistant: role = "assistant"; break;
      case firmius::shared::Role::System: role = "system"; break;
      case firmius::shared::Role::ToolResult: role = "tool"; break;
      default: role = "user"; break;
      }
      auto m = encodeRemoteApiChatMessage(role, text, toolCallId);
      writeMessageField(remoteApiReq, 4, m);
    }
  }
  prependGrpcFrame(remoteApiReq);

  // ---- path[0]: ApiServerService.GetStreamingExternalChatCompletions ----
  // Different request type entirely (GetChatCompletionsRequest, not
  // GetChatMessageRequest). Verified against the embedded FileDescriptorProto
  // in language_server_linux_x64. Streams a CompletionDeltaMap per frame.
  std::string compReq;
  writeMessageField(compReq, 1, metadata);             // metadata
  // Field 2: repeated ChatMessagePrompt — reuse the same prompt structure as
  // path[2]. Server requires at least one structured prompt.
  writeMessageField(compReq, 2, promptMsg);
  if (!systemPrompt.empty()) {
    writeStringField(compReq, 3, systemPrompt);        // system_prompt
  }
  // Field 4: CompletionsRequest — embeds the legacy completion config plus
  // the model selector (enum field 10 + string field 17 model_tag for dynamic
  // UIDs).
  std::string compCfg;
  std::string compInner;
  writeVarintField(compInner, 2, 8192);                // configuration.max_tokens
  writeFixed64Field(compInner, 5,
                    static_cast<double>(opts.temperature));
  writeMessageField(compCfg, 1, compInner);            // configuration
  writeStringField(compCfg, 2, apiPrompt);             // prompt
  writeVarintField(compCfg, 4, 30);                    // language=PLAINTEXT
  if (resolved->enumValue > 0) {
    writeVarintField(compCfg, 10,
                     static_cast<std::uint64_t>(resolved->enumValue));
  } else {
    writeStringField(compCfg, 17, resolved->wireModelName); // model_tag
  }
  writeStringField(compCfg, 25, requestId);            // uid (per-request)
  writeMessageField(compReq, 4, compCfg);
  writeVarintField(compReq, 5, 2);                     // PROVIDER_SOURCE_CHAT
  if (resolved->enumValue > 0) {
    writeVarintField(compReq, 6,
                     static_cast<std::uint64_t>(resolved->enumValue));
  }
  writeStringField(compReq, 9, requestId);             // prompt_id
  prependGrpcFrame(compReq);

  // ---- Try service paths until one succeeds ----
  std::string accumulated;
  StreamCtx ctx;
  ctx.abort = opts.abortSignal;
  ctx.onText = [&](std::string_view text) {
    accumulated.append(text);
    if (!toolsActive) {
      // Pass through to the user immediately.
      onEvent(firmius::shared::TextChunk{std::string(text)});
    }
    // When tools are active we accumulate and parse once at end.
  };

  std::string lastErr;
  bool ok = false;
  int hint = g_chatPathIndex.load();
  auto tryAt = [&](int i) {
    if (i < 0 || i >= static_cast<int>(std::size(kChatPaths))) return false;
    ctx.buffer.clear();
    ctx.consumed = 0;
    ctx.grpcStatus = -1;
    ctx.grpcMessage.clear();
    accumulated.clear();
    std::string url = std::string(kChatPaths[i].base) + kChatPaths[i].path;
    std::string err;
    const std::string &payload =
        (i == 0) ? compReq : (i == 2 ? remoteApiReq : req);
    if (debug) {
      std::fprintf(stderr, "[windsurf-remote] try path[%d] %s payload=%zu\n",
                   i, url.c_str(), payload.size());
    }
    bool success = runGrpcStream(url, apiKey, payload, ctx, err);
    if (success) {
      if (debug) {
        std::fprintf(stderr, "[windsurf-remote] path[%d] ok\n", i);
      }
      g_chatPathIndex.store(i);
      return true;
    }
    if (debug) {
      std::fprintf(stderr, "[windsurf-remote] path[%d] failed: %s\n", i,
                   err.c_str());
    }
    lastErr = err;
    return false;
  };

  if (hint >= 0) ok = tryAt(hint);
  if (!ok) {
    for (int i = 0; i < static_cast<int>(std::size(kChatPaths)); ++i) {
      if (i == hint) continue;
      if (tryAt(i)) { ok = true; break; }
    }
  }

  if (!ok) {
    onEvent(firmius::shared::StreamError{
        "Windsurf chat call failed: " + lastErr, 502, ""});
    onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Error});
    return;
  }

  if (toolsActive) {
    auto plan = parseToolPlan(accumulated);
    if (plan.isToolCall) {
      for (std::size_t i = 0; i < plan.calls.size(); ++i) {
        const auto &c = plan.calls[i];
        std::string id = "call_" + std::to_string(i) + "_" +
                         std::to_string(std::rand());
        // Emit progressive ToolCallChunk so live UIs animate, then the final
        // ToolCall record.
        onEvent(firmius::shared::ToolCallChunk{
            id, static_cast<std::uint32_t>(i), c.name, c.argsJson});
        onEvent(firmius::shared::ToolCall{
            id, static_cast<std::uint32_t>(i), c.name, c.argsJson});
      }
      onEvent(firmius::shared::StreamDone{
          firmius::shared::StopReason::ToolUse});
      return;
    }
    if (!plan.finalContent.empty()) {
      onEvent(firmius::shared::TextChunk{plan.finalContent});
    }
  }

  onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Stop});
}

void WindsurfProvider::generateSummary(
    const std::string &modelId, const firmius::shared::AgentHistory &history,
    const std::string &compactionPrompt,
    std::function<void(const firmius::shared::StreamEvent &)> onEvent,
    std::atomic<bool> *abortSignal) {
  // Re-use stream() with a synthesized history that ends in a user turn.
  ProviderOptions opts;
  opts.modelId = modelId;
  opts.temperature = 0.1f;
  opts.abortSignal = abortSignal;

  firmius::shared::AgentHistory sumHist = history;
  firmius::shared::AgentTurn summaryTurn;
  firmius::shared::Message summaryUser;
  summaryUser.role = firmius::shared::Role::User;
  summaryUser.content.push_back(
      firmius::shared::TextContent{compactionPrompt});
  summaryTurn.messages.push_back(std::move(summaryUser));
  sumHist.turns.push_back(std::move(summaryTurn));

  // Wrap output to emit AgentCompactionText instead of TextChunk so the
  // compaction view picks it up.
  stream(sumHist, opts, [&](const firmius::shared::StreamEvent &ev) {
    if (auto *t = std::get_if<firmius::shared::TextChunk>(&ev)) {
      onEvent(firmius::shared::AgentCompactionText{"", t->delta, ""});
    } else {
      onEvent(ev);
    }
  });
}

// ===========================================================================
// Quota tracking
// ===========================================================================

bool WindsurfProvider::fetchUserStatus(
    const firmius::shared::OAuthAccount &acc, AccountQuota &outQuota,
    std::string &outError) {
  // Best-effort: try the documented service-key endpoint shape first, then
  // the legacy api.codeium.com paths. We don't know the exact user-status
  // route for individual accounts (only the enterprise service-key API is
  // public), so we surface what we can find.
  static constexpr const char *kStatusUrls[] = {
      "https://server.codeium.com/exa.seat_management_pb."
      "SeatManagementService/GetUserStatus",
      "https://api.codeium.com/get_user_status",
  };
  for (const char *url : kStatusUrls) {
    std::string resp;
    if (postJson(url, acc.accessToken, "{}", resp)) {
      rapidjson::Document doc;
      doc.Parse(resp.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("plan_tier") && doc["plan_tier"].IsString()) {
          outQuota.planTier = doc["plan_tier"].GetString();
        } else if (doc.HasMember("planTier") && doc["planTier"].IsString()) {
          outQuota.planTier = doc["planTier"].GetString();
        } else if (doc.HasMember("subscription") &&
                   doc["subscription"].IsObject() &&
                   doc["subscription"].HasMember("tier") &&
                   doc["subscription"]["tier"].IsString()) {
          outQuota.planTier = doc["subscription"]["tier"].GetString();
        }
        if (doc.HasMember("subscription_expiry") &&
            doc["subscription_expiry"].IsString()) {
          outQuota.subscriptionExpiry =
              doc["subscription_expiry"].GetString();
        }
        return true;
      }
    }
  }
  outError = "user-status unavailable";
  return false;
}

bool WindsurfProvider::fetchUsageLimits(
    const firmius::shared::OAuthAccount &acc, AccountQuota &outQuota,
    std::string &outError) {
  static constexpr const char *kUsageUrls[] = {
      "https://server.codeium.com/exa.api_server_pb.ApiServerService/"
      "GetUsageLimits",
      "https://api.codeium.com/get_usage_limits",
  };
  for (const char *url : kUsageUrls) {
    std::string resp;
    if (postJson(url, acc.accessToken, "{}", resp)) {
      rapidjson::Document doc;
      doc.Parse(resp.c_str());
      if (doc.HasParseError() || !doc.IsObject()) continue;

      auto readInt = [&](const char *a, const char *b) -> std::int64_t {
        if (doc.HasMember(a)) {
          const auto &v = doc[a];
          if (v.IsInt64()) return v.GetInt64();
          if (v.IsInt()) return v.GetInt();
        }
        if (doc.HasMember(b)) {
          const auto &v = doc[b];
          if (v.IsInt64()) return v.GetInt64();
          if (v.IsInt()) return v.GetInt();
        }
        return 0;
      };
      outQuota.promptCreditsUsed =
          readInt("prompt_credits_used", "promptCreditsUsed");
      outQuota.promptCreditsLimit =
          readInt("prompt_credits_limit", "promptCreditsLimit");
      outQuota.flowActionsUsed =
          readInt("flow_actions_used", "flowActionsUsed");
      outQuota.flowActionsLimit =
          readInt("flow_actions_limit", "flowActionsLimit");
      outQuota.resetEpochSeconds =
          readInt("reset_at", "resetAt");

      if (doc.HasMember("per_model") && doc["per_model"].IsObject()) {
        for (auto it = doc["per_model"].MemberBegin();
             it != doc["per_model"].MemberEnd(); ++it) {
          if (!it->name.IsString() || !it->value.IsObject()) continue;
          std::int64_t used = 0, limit = 0;
          if (it->value.HasMember("used") && it->value["used"].IsInt64())
            used = it->value["used"].GetInt64();
          if (it->value.HasMember("limit") && it->value["limit"].IsInt64())
            limit = it->value["limit"].GetInt64();
          outQuota.perModel[it->name.GetString()] = {used, limit};
        }
      }
      return true;
    }
  }
  outError = "usage-limits unavailable";
  return false;
}

namespace {

// Decode SeatManagementService.GetPlanStatusResponse against an authenticated
// account. Wire shape verified live against an authed dashboard session.
//
// Request body:  GetPlanStatusRequest { 1 auth_token (string),
//                                       2 include_top_up_status (bool) }
// Response: GetPlanStatusResponse {
//   1 plan_status (codeium_common_pb.PlanStatus),
//   2 team_used_prompt_credits (int64)
// }
// PlanStatus carries the user-visible numbers we want:
//   1  plan_info { 2 plan_name, 12 monthly_prompt_credits, ... }
//   2  plan_start (timestamp)
//   3  plan_end (timestamp)
//   5  used_flow_credits          (int)
//   6  used_prompt_credits        (int)
//   8  available_prompt_credits   (int)
//   9  available_flow_credits     (int)
//   14 daily_quota_remaining_percent  (int)
//   15 weekly_quota_remaining_percent (int)
//   16 overage_balance_micros         (int64; $1.234 = 1_234_000)
//   17 daily_quota_reset_at_unix
//   18 weekly_quota_reset_at_unix
bool fetchPlanStatusViaConnect(const firmius::shared::OAuthAccount &acc,
                               WindsurfProvider::AccountQuota &out,
                               std::string &outError) {
  std::string body;
  writeStringField(body, 1, acc.accessToken); // auth_token
  writeVarintField(body, 2, 1);               // include_top_up_status

  std::string respBytes;
  if (!runConnectUnary(
          "/exa.seat_management_pb.SeatManagementService/GetPlanStatus",
          acc.accessToken, body, respBytes, outError)) {
    return false;
  }

  // Default to "100% remaining" so the modal always shows daily+weekly
  // buckets even if the server omits the percent fields for newly created
  // accounts (we still flag the call as successful below).
  out.dailyLimit = 100;
  out.dailyUsed = 0;
  out.weeklyLimit = 100;
  out.weeklyUsed = 0;

  std::size_t off = 0;
  while (off < respBytes.size()) {
    ParsedField f;
    if (!parseField(reinterpret_cast<const std::uint8_t *>(respBytes.data() + off),
                    respBytes.size() - off, f)) {
      break;
    }
    off += f.consumed;
    if (f.fieldNum != 1 || f.wireType != 2) continue;

    // PlanStatus body
    std::size_t io = 0;
    while (io < f.bytes.size()) {
      ParsedField pf;
      if (!parseField(
              reinterpret_cast<const std::uint8_t *>(f.bytes.data() + io),
              f.bytes.size() - io, pf)) {
        break;
      }
      io += pf.consumed;
      switch (pf.fieldNum) {
      case 1: { // plan_info
        if (pf.wireType != 2) break;
        std::size_t po = 0;
        while (po < pf.bytes.size()) {
          ParsedField ipf;
          if (!parseField(reinterpret_cast<const std::uint8_t *>(
                              pf.bytes.data() + po),
                          pf.bytes.size() - po, ipf)) {
            break;
          }
          po += ipf.consumed;
          if (ipf.fieldNum == 2 && ipf.wireType == 2) {
            out.planTier.assign(ipf.bytes.data(), ipf.bytes.size());
          }
        }
        break;
      }
      case 5:
        if (pf.wireType == 0) out.flowActionsUsed = pf.varint;
        break;
      case 6:
        if (pf.wireType == 0) out.promptCreditsUsed = pf.varint;
        break;
      case 8:
        if (pf.wireType == 0) out.promptCreditsLimit = pf.varint;
        break;
      case 9:
        if (pf.wireType == 0) out.flowActionsLimit = pf.varint;
        break;
      case 14:
        if (pf.wireType == 0) {
          out.dailyUsed = 100 - static_cast<std::int64_t>(pf.varint);
        }
        break;
      case 15:
        if (pf.wireType == 0) {
          out.weeklyUsed = 100 - static_cast<std::int64_t>(pf.varint);
        }
        break;
      case 16:
        if (pf.wireType == 0) {
          out.extraUsageBalanceUsd =
              static_cast<double>(pf.varint) / 1'000'000.0;
        }
        break;
      case 17:
        if (pf.wireType == 0) out.dailyResetEpochSeconds = pf.varint;
        break;
      case 18:
        if (pf.wireType == 0) out.weeklyResetEpochSeconds = pf.varint;
        break;
      default:
        break;
      }
    }
  }

  // We got a 200 from the server — even if specific percent fields were
  // missing for this account, the default 100% values above are populated,
  // so we always succeed here and skip the legacy fallbacks.
  return true;
}

} // namespace

void WindsurfProvider::refreshQuotas() {
  auto accounts = getAccounts();
  std::map<std::string, AccountQuota> next;
  for (const auto &acc : accounts) {
    AccountQuota q;
    std::string err;
    // Single source of truth: windsurf.com/_backend GetPlanStatus. If it
    // hard-fails we still emit defaulted (100%) daily/weekly buckets so
    // every account is represented in the TUI quota modal.
    fetchPlanStatusViaConnect(acc, q, err);
    if (q.dailyLimit == 0) {
      q.dailyLimit = 100;
      q.dailyUsed = 0;
    }
    if (q.weeklyLimit == 0) {
      q.weeklyLimit = 100;
      q.weeklyUsed = 0;
    }
    next[acc.identifier] = std::move(q);
  }
  std::lock_guard<std::mutex> lock(quotaMutex_);
  quotas_ = std::move(next);
}

std::map<std::string, std::vector<firmius::shared::QuotaBucket>>
WindsurfProvider::getAllQuotas() const {
  std::lock_guard<std::mutex> lock(quotaMutex_);
  std::map<std::string, std::vector<firmius::shared::QuotaBucket>> out;
  // Per product decision: surface ONLY the daily and weekly buckets per
  // account. The Windsurf dashboard itself only shows these two for the
  // free / trial tiers, and other "buckets" (plan tier label, prompt
  // credits, flow actions, top-up balance) confused the UI by showing the
  // first row at a misleading 100%. Both buckets are always emitted so
  // every account is represented in the modal.
  auto makePctBucket = [](const char *name, std::int64_t used,
                          std::int64_t limit, std::int64_t resetEpoch) {
    firmius::shared::QuotaBucket b;
    b.name = name;
    if (limit <= 0) limit = 100;
    float remaining =
        static_cast<float>(limit - used) / static_cast<float>(limit);
    b.remainingFraction = std::clamp(remaining, 0.0f, 1.0f);
    int pct = static_cast<int>(b.remainingFraction * 100.0f + 0.5f);
    b.note = std::to_string(pct) + "% remaining";
    if (resetEpoch > 0) {
      b.resetTime = std::to_string(resetEpoch);
    }
    return b;
  };

  for (const auto &[id, q] : quotas_) {
    std::vector<firmius::shared::QuotaBucket> buckets;
    buckets.push_back(makePctBucket("daily", q.dailyUsed, q.dailyLimit,
                                    q.dailyResetEpochSeconds));
    buckets.push_back(makePctBucket("weekly", q.weeklyUsed, q.weeklyLimit,
                                    q.weeklyResetEpochSeconds));
    out[id] = std::move(buckets);
  }
  return out;
}

// ===========================================================================
// Model discovery
// ===========================================================================

std::size_t WindsurfProvider::fetchAndMergeModels(
    const firmius::shared::OAuthAccount &acc) {
  // Path A — windsurf.com/_backend Connect-RPC
  // (`exa.api_server_pb.ApiServerService/GetCascadeModelConfigs`).
  // This is the same call the windsurf.com dashboard makes; verified live to
  // return 90+ entries including Adaptive / SWE-1.6 / Claude Opus 4.7 etc.
  {
    std::string sessionId =
        acc.metadata.count("sessionId") ? acc.metadata.at("sessionId") : "";
    if (sessionId.empty()) {
      sessionId = std::to_string(
          std::chrono::system_clock::now().time_since_epoch().count());
    }
    std::string md = encodeBackendMetadata(acc.accessToken, sessionId);
    std::string req;
    writeMessageField(req, 1, md); // GetCascadeModelConfigsRequest.metadata

    std::string body;
    std::string err;
    bool ok = runConnectUnary(
        "/exa.api_server_pb.ApiServerService/GetCascadeModelConfigs",
        acc.accessToken, req, body, err);
    if (ok && !body.empty()) {
      // Response: GetCascadeModelConfigsResponse {
      //   1 client_model_configs (repeated ClientModelConfig)
      //   2 client_model_sorts
      //   3 default_override_model_config
      // }
      // ClientModelConfig fields we care about:
      //   1  label (string, e.g. "Claude Opus 4.7 Medium")
      //   3  credit_multiplier (float)
      //   4  disabled (bool)
      //   7  is_premium
      //   18 max_tokens (int)
      //   22 model_uid (string, e.g. "claude-opus-4-7-medium")
      //   2  model_or_alias { 1 enum_value }
      //   23 model_info     { 4 context_window, 13 max_output_tokens }
      std::vector<CachedModel> discovered;
      std::size_t off = 0;
      while (off < body.size()) {
        ParsedField f;
        if (!parseField(reinterpret_cast<const std::uint8_t *>(body.data() + off),
                        body.size() - off, f)) {
          break;
        }
        off += f.consumed;
        if (f.fieldNum != 1 || f.wireType != 2) continue;

        CachedModel cm;
        cm.fromDiscovery = true;

        std::size_t io = 0;
        while (io < f.bytes.size()) {
          ParsedField mf;
          if (!parseField(reinterpret_cast<const std::uint8_t *>(
                              f.bytes.data() + io),
                          f.bytes.size() - io, mf)) {
            break;
          }
          io += mf.consumed;
          if (mf.fieldNum == 1 && mf.wireType == 2) {
            cm.displayName.assign(mf.bytes.data(), mf.bytes.size());
          } else if (mf.fieldNum == 22 && mf.wireType == 2) {
            cm.canonicalId.assign(mf.bytes.data(), mf.bytes.size());
          } else if (mf.fieldNum == 18 && mf.wireType == 0) {
            cm.maxOutput = static_cast<std::uint32_t>(mf.varint);
          } else if (mf.fieldNum == 3 && mf.wireType == 5) {
            float v;
            std::memcpy(&v, mf.bytes.data(), 4);
            cm.creditMultiplier = static_cast<double>(v);
          } else if (mf.fieldNum == 4 && mf.wireType == 0) {
            // disabled — skip
            if (mf.varint != 0) {
              cm.canonicalId.clear();
              break;
            }
          } else if (mf.fieldNum == 5 && mf.wireType == 0) {
            cm.supportsImages = (mf.varint != 0);
          } else if (mf.fieldNum == 2 && mf.wireType == 2) {
            // model_or_alias { 1 enum_value }
            std::size_t mo = 0;
            while (mo < mf.bytes.size()) {
              ParsedField af;
              if (!parseField(reinterpret_cast<const std::uint8_t *>(
                                  mf.bytes.data() + mo),
                              mf.bytes.size() - mo, af)) {
                break;
              }
              mo += af.consumed;
              if (af.fieldNum == 1 && af.wireType == 0) {
                cm.enumValue = static_cast<int>(af.varint);
              }
            }
          } else if (mf.fieldNum == 23 && mf.wireType == 2) {
            // model_info — wire layout (verified live 2026-04-27 against
            // GPT-5.5 = 272K, Opus 4.7 = 1M):
            //   field 4  = context_window  (varint, total budget incl. output)
            //   field 13 = max_output_tokens (varint, budget reserved for the
            //              completion side of that window)
            // The previous comment had these backwards which is why every
            // model showed up as ctx=128000 in the discovery audit while the
            // real value was being stored in `maxOutput`.
            std::size_t mo = 0;
            while (mo < mf.bytes.size()) {
              ParsedField af;
              if (!parseField(reinterpret_cast<const std::uint8_t *>(
                                  mf.bytes.data() + mo),
                              mf.bytes.size() - mo, af)) {
                break;
              }
              mo += af.consumed;
              if (af.fieldNum == 4 && af.wireType == 0) {
                cm.contextWindow = static_cast<std::uint32_t>(af.varint);
              } else if (af.fieldNum == 13 && af.wireType == 0) {
                cm.maxOutput = static_cast<std::uint32_t>(af.varint);
              }
            }
          } else if (mf.fieldNum == 30 && mf.wireType == 2) {
            // model_family_metadata { 1 family_name, 2 dimensions [
            //   { 1 dimension_name, 2 options [ { 1 v, 2 label } ] } ] }
            // We surface each dimension's option labels as variant strings,
            // formatted as "DimensionName: OptionLabel" so the picker can
            // group them logically.
            std::size_t mo = 0;
            std::string familyName;
            while (mo < mf.bytes.size()) {
              ParsedField af;
              if (!parseField(reinterpret_cast<const std::uint8_t *>(
                                  mf.bytes.data() + mo),
                              mf.bytes.size() - mo, af)) {
                break;
              }
              mo += af.consumed;
              if (af.fieldNum == 1 && af.wireType == 2) {
                familyName.assign(af.bytes.data(), af.bytes.size());
              } else if (af.fieldNum == 2 && af.wireType == 2) {
                std::string dimName;
                std::vector<std::string> options;
                std::size_t do_ = 0;
                while (do_ < af.bytes.size()) {
                  ParsedField df;
                  if (!parseField(reinterpret_cast<const std::uint8_t *>(
                                      af.bytes.data() + do_),
                                  af.bytes.size() - do_, df)) {
                    break;
                  }
                  do_ += df.consumed;
                  if (df.fieldNum == 1 && df.wireType == 2) {
                    dimName.assign(df.bytes.data(), df.bytes.size());
                  } else if (df.fieldNum == 2 && df.wireType == 2) {
                    // option { 1 v, 2 label }
                    std::size_t oo = 0;
                    while (oo < df.bytes.size()) {
                      ParsedField of;
                      if (!parseField(reinterpret_cast<const std::uint8_t *>(
                                          df.bytes.data() + oo),
                                      df.bytes.size() - oo, of)) {
                        break;
                      }
                      oo += of.consumed;
                      if (of.fieldNum == 2 && of.wireType == 2) {
                        options.emplace_back(of.bytes.data(),
                                             of.bytes.size());
                      }
                    }
                  }
                }
                if (!options.empty() && !dimName.empty()) {
                  for (auto &opt : options) {
                    cm.variants.push_back(dimName + ": " + opt);
                  }
                } else if (!dimName.empty()) {
                  cm.variants.push_back(dimName);
                }
              }
            }
          } else if (mf.fieldNum == 32 && mf.wireType == 2) {
            // model_dimensions (repeated): { 1 name, 2 value (f32),
            //   3 unit, 4 min (f32), 5 max (f32), 6 v, 7 description }
            // We use {name, value, unit} to extract input/output/cache
            // pricing per 1M tokens.
            std::string dname, dunit;
            float dvalue = 0.0f;
            bool haveValue = false;
            std::size_t mo = 0;
            while (mo < mf.bytes.size()) {
              ParsedField af;
              if (!parseField(reinterpret_cast<const std::uint8_t *>(
                                  mf.bytes.data() + mo),
                              mf.bytes.size() - mo, af)) {
                break;
              }
              mo += af.consumed;
              if (af.fieldNum == 1 && af.wireType == 2) {
                dname.assign(af.bytes.data(), af.bytes.size());
              } else if (af.fieldNum == 2 && af.wireType == 5) {
                std::memcpy(&dvalue, af.bytes.data(), 4);
                haveValue = true;
              } else if (af.fieldNum == 3 && af.wireType == 2) {
                dunit.assign(af.bytes.data(), af.bytes.size());
              }
            }
            if (haveValue && dunit.find("1M") != std::string::npos) {
              auto lower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                return s;
              };
              std::string n = lower(dname);
              if (n == "input") {
                cm.pricePer1MInput = static_cast<double>(dvalue);
              } else if (n == "output") {
                cm.pricePer1MOutput = static_cast<double>(dvalue);
              } else if (n == "cached input" || n == "cache read") {
                cm.pricePer1MCacheRead = static_cast<double>(dvalue);
              } else if (n == "cache write" || n == "cached output") {
                cm.pricePer1MCacheWrite = static_cast<double>(dvalue);
              }
            }
          }
        }

        if (cm.canonicalId.empty()) continue;
        if (cm.displayName.empty()) cm.displayName = cm.canonicalId;
        if (cm.contextWindow == 0) cm.contextWindow = 200000;
        if (cm.maxOutput == 0) cm.maxOutput = 8192;
        discovered.push_back(std::move(cm));
      }

      if (!discovered.empty()) {
        std::lock_guard<std::recursive_mutex> lock(modelMutex_);
        // Discovery is the authoritative catalog. Replace EVERYTHING in the
        // cache (both prior discovery results and the legacy static fallback)
        // with the live server response so the picker never shows stale or
        // enum-named placeholders alongside real models. Discovery success
        // also marks the lazy listModels() hook as satisfied so we don't
        // race a second background fetch immediately afterward.
        models_.clear();
        discoveryStarted_.store(true);
        for (auto &cm : discovered) {
          auto it = std::find_if(models_.begin(), models_.end(),
                                 [&](const CachedModel &x) {
                                   return x.canonicalId == cm.canonicalId;
                                 });
          if (it != models_.end()) {
            *it = std::move(cm);
          } else {
            models_.push_back(std::move(cm));
          }
        }
        saveModelCache();
        return discovered.size();
      }
    }
  }

  // Path B — legacy gRPC GetModelStatuses on server.codeium.com (kept for
  // resilience if the dashboard backend is ever unreachable).
  {
    auto discovered = discoverModelsViaGrpc(acc);
    if (!discovered.empty()) {
      std::lock_guard<std::recursive_mutex> lock(modelMutex_);
      for (auto &cm : discovered) {
        CachedModel cacheModel;
        cacheModel.enumValue = cm.enumValue;
        cacheModel.canonicalId = cm.canonicalId;
        cacheModel.displayName = cm.canonicalId;
        cacheModel.contextWindow = 200000;
        cacheModel.maxOutput = 8192;
        cacheModel.fromDiscovery = true;

        auto it = std::find_if(models_.begin(), models_.end(),
                               [&](const CachedModel &x) {
                                 return x.canonicalId == cacheModel.canonicalId;
                               });
        if (it != models_.end()) {
          *it = std::move(cacheModel);
        } else {
          models_.push_back(std::move(cacheModel));
        }
      }
      saveModelCache();
      return discovered.size();
    }
  }

  // Try cloud-side model list endpoints — these aren't formally documented
  // for individual users, so we attempt a small set in priority order and
  // accept the first that returns parseable JSON or protobuf.
  static constexpr const char *kModelUrls[] = {
      "https://server.codeium.com/exa.api_server_pb.ApiServerService/"
      "GetCascadeModelConfigs",
      "https://server.codeium.com/exa.language_server_pb."
      "LanguageServerService/GetModelStatuses",
      "https://api.codeium.com/get_models",
  };

  std::vector<CachedModel> discovered;
  for (const char *url : kModelUrls) {
    std::string resp;
    if (!postJson(url, acc.accessToken, "{}", resp)) continue;
    rapidjson::Document doc;
    doc.Parse(resp.c_str());
    if (doc.HasParseError() || !doc.IsObject()) continue;

    // Common shape variants: {models: [...]}, {data: [...]}, top-level array.
    const rapidjson::Value *arr = nullptr;
    if (doc.HasMember("models") && doc["models"].IsArray())
      arr = &doc["models"];
    else if (doc.HasMember("data") && doc["data"].IsArray())
      arr = &doc["data"];
    else if (doc.HasMember("model_status_infos") &&
             doc["model_status_infos"].IsArray())
      arr = &doc["model_status_infos"];

    if (!arr) continue;
    for (const auto &m : arr->GetArray()) {
      if (!m.IsObject()) continue;
      CachedModel cm;
      // enum + canonical id
      if (m.HasMember("model") && m["model"].IsInt())
        cm.enumValue = m["model"].GetInt();
      else if (m.HasMember("enum") && m["enum"].IsInt())
        cm.enumValue = m["enum"].GetInt();
      else if (m.HasMember("enum_value") && m["enum_value"].IsInt())
        cm.enumValue = m["enum_value"].GetInt();
      if (m.HasMember("model_uid") && m["model_uid"].IsString())
        cm.canonicalId = m["model_uid"].GetString();
      else if (m.HasMember("id") && m["id"].IsString())
        cm.canonicalId = m["id"].GetString();
      else if (m.HasMember("modelId") && m["modelId"].IsString())
        cm.canonicalId = m["modelId"].GetString();
      if (m.HasMember("display_name") && m["display_name"].IsString())
        cm.displayName = m["display_name"].GetString();
      else if (m.HasMember("name") && m["name"].IsString())
        cm.displayName = m["name"].GetString();
      if (m.HasMember("context_window") && m["context_window"].IsInt())
        cm.contextWindow = m["context_window"].GetUint();
      if (m.HasMember("max_output_tokens") &&
          m["max_output_tokens"].IsInt())
        cm.maxOutput = m["max_output_tokens"].GetUint();
      if (m.HasMember("supports_reasoning") &&
          m["supports_reasoning"].IsBool())
        cm.supportsReasoning = m["supports_reasoning"].GetBool();
      if (m.HasMember("variants") && m["variants"].IsArray()) {
        for (const auto &v : m["variants"].GetArray()) {
          if (v.IsString()) {
            cm.variants.push_back(v.GetString());
          } else if (v.IsObject() && v.HasMember("id") &&
                     v["id"].IsString()) {
            cm.variants.push_back(v["id"].GetString());
          }
        }
      }
      cm.fromDiscovery = true;
      if (cm.canonicalId.empty() || cm.enumValue == 0) continue;
      discovered.push_back(std::move(cm));
    }
    if (!discovered.empty()) break;
  }

  if (discovered.empty()) return 0;

  {
    std::lock_guard<std::recursive_mutex> lock(modelMutex_);
    for (auto &cm : discovered) {
      auto it = std::find_if(models_.begin(), models_.end(),
                             [&](const CachedModel &x) {
                               return x.canonicalId == cm.canonicalId;
                             });
      if (it != models_.end()) {
        *it = std::move(cm);
      } else {
        models_.push_back(std::move(cm));
      }
    }
  }
  saveModelCache();
  return discovered.size();
}

} // namespace firmius::provider
