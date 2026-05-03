#include "providers/KiroProvider.hpp"
#include "utils/GCPHttpClient.hpp"
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

std::uint64_t fnv1a64(const std::string &value) {
  constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffsetBasis;
  for (unsigned char ch : value) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kPrime;
  }
  return hash;
}

std::string stableKiroAccountIdentifier(const std::string &authMethod,
                                        const std::string &email,
                                        const std::string &clientId,
                                        const std::string &profileArn) {
  std::ostringstream ss;
  ss << "kiro-" << std::hex
     << fnv1a64(email + "|" + authMethod + "|" + clientId + "|" + profileArn);
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

std::string generateUuid() {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 15);
  std::stringstream ss;
  ss << std::hex;
  for (int i = 0; i < 8; ++i) ss << dist(rd);
  ss << "-";
  for (int i = 0; i < 4; ++i) ss << dist(rd);
  ss << "-4";
  for (int i = 0; i < 3; ++i) ss << dist(rd);
  ss << "-";
  ss << (dist(rd) % 4 + 8);
  for (int i = 0; i < 3; ++i) ss << dist(rd);
  ss << "-";
  for (int i = 0; i < 12; ++i) ss << dist(rd);
  return ss.str();
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
  if (const char *home = std::getenv("HOME"); home && *home) {
    return home;
  }
  return {};
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
    out.write(data, static_cast<std::streamsize>(size));
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

} // namespace

// Static model definitions
std::vector<KiroProvider::KiroModel> KiroProvider::getKiroModels() {
  return {
      {"claude-sonnet-4.5", "Claude Sonnet 4.5", 1.30, 200000, 64000, {"text", "image", "pdf"}, false},
      {"claude-sonnet-4", "Claude Sonnet 4", 1.30, 200000, 64000, {"text", "image", "pdf"}, false},
      {"claude-haiku-4.5", "Claude Haiku 4.5", 0.40, 200000, 64000, {"text", "image"}, false},
      {"deepseek-3.2", "DeepSeek V3.2", 0.25, 200000, 64000, {"text"}, false},
      {"minimax-m2.5", "MiniMax M2.5", 0.25, 200000, 64000, {"text"}, false},
      {"minimax-m2.1", "MiniMax M2.1", 0.15, 200000, 64000, {"text"}, false},
      {"glm-5", "GLM-5", 0.50, 200000, 64000, {"text"}, false},
      {"qwen3-coder-next", "Qwen3 Coder Next", 0.05, 200000, 64000, {"text"}, false},
  };
}

std::string KiroProvider::resolveModelId(const std::string &modelId) {
  // Map common aliases to Kiro model IDs
  static const std::map<std::string, std::string> aliases = {
      {"claude-sonnet-4-5", "claude-sonnet-4.5"},
      {"claude-haiku-4-5", "claude-haiku-4.5"},
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

  rapidjson::Document respDoc;
  respDoc.Parse(response.body.c_str());
  if (respDoc.HasParseError() || !respDoc.IsObject()) {
    return false;
  }

  int usedCount = 0;
  int limitCount = 0;

  if (respDoc.HasMember("usageBreakdownList") && respDoc["usageBreakdownList"].IsArray()) {
    for (const auto &item : respDoc["usageBreakdownList"].GetArray()) {
      if (item.HasMember("freeTrialInfo") && item["freeTrialInfo"].IsObject()) {
        auto &ft = item["freeTrialInfo"];
        if (ft.HasMember("currentUsage")) usedCount += ft["currentUsage"].GetInt();
        if (ft.HasMember("usageLimit")) limitCount += ft["usageLimit"].GetInt();
      }
      if (item.HasMember("currentUsage")) usedCount += item["currentUsage"].GetInt();
      if (item.HasMember("usageLimit")) limitCount += item["usageLimit"].GetInt();
    }
  }

  acc.metadata["usedCount"] = std::to_string(usedCount);
  acc.metadata["limitCount"] = std::to_string(limitCount);

  if (respDoc.HasMember("userInfo") && respDoc["userInfo"].IsObject()) {
    auto &ui = respDoc["userInfo"];
    if (ui.HasMember("email") && ui["email"].IsString()) {
      acc.metadata["email"] = ui["email"].GetString();
    }
  }

  acc.lastQuotaRefresh = nowSeconds();
  return true;
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
      usedCount = std::stoi(acc.metadata.at("usedCount"));
    }
    if (acc.metadata.count("limitCount")) {
      limitCount = std::stoi(acc.metadata.at("limitCount"));
    }

    if (limitCount > 0) {
      bucket.remainingFraction = static_cast<float>(limitCount - usedCount) / limitCount;
    }
    bucket.note = std::to_string(usedCount) + "/" + std::to_string(limitCount) + " requests";

    result[acc.identifier] = {bucket};
  }

  return result;
}

std::optional<OAuthAccount> KiroProvider::getAvailableAccount(const std::optional<std::string> &modelId) {
  return BaseOAuthProvider::getAvailableAccount(modelId);
}

void KiroProvider::stream(const AgentHistory &history, const ProviderOptions &opts,
                          std::function<void(const StreamEvent &)> onEvent) {
  auto accOpt = getAvailableAccount(opts.modelId);
  if (!accOpt) {
    onEvent(StreamError{"No Kiro account available. Run /connect kiro to authenticate.", 401, ""});
    onEvent(StreamDone{StopReason::Error});
    return;
  }

  OAuthAccount acc = *accOpt;
  std::string requestBody = buildCodeWhispererRequest(history, opts.modelId, acc, opts);

  std::string region = acc.metadata.count("region") ? acc.metadata["region"] : kDefaultRegion;
  std::string apiUrl = buildUrl("https://q.{{region}}.amazonaws.com/generateAssistantResponse", region);

  std::string profileArn = acc.metadata.count("profileArn") ? acc.metadata["profileArn"] : "";

  StreamContext ctx;
  ctx.provider = this;
  ctx.onEvent = &onEvent;
  ctx.abortSignal = opts.abortSignal;

  CURL *curl = curl_easy_init();
  if (!curl) {
    onEvent(StreamError{"Failed to initialize CURL", 500, acc.identifier});
    onEvent(StreamDone{StopReason::Error});
    return;
  }

  struct curl_slist *chunk = nullptr;
  chunk = curl_slist_append(chunk, "Content-Type: application/json");
  chunk = curl_slist_append(chunk, "Accept: application/json");
  chunk = curl_slist_append(chunk, ("Authorization: Bearer " + acc.accessToken).c_str());
  chunk = curl_slist_append(chunk, ("amz-sdk-invocation-id: " + generateUuid()).c_str());
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
    onEvent(StreamError{curl_easy_strerror(res), 0, acc.identifier});
    onEvent(StreamDone{StopReason::Error});
    return;
  }

  if (httpStatus == 429) {
    markAccountRateLimited(acc, 60);
    onEvent(StreamError{"Rate limited", 429, acc.identifier});
    onEvent(StreamDone{StopReason::Error});
    return;
  }

  if (httpStatus >= 400) {
    std::string msg = "HTTP error " + std::to_string(httpStatus);
    if (!ctx.buffer.empty()) {
      std::string snippet = ctx.buffer;
      if (snippet.size() > 2000) {
        snippet.resize(2000);
        snippet += "...";
      }
      msg += " body=" + snippet;
    }
    onEvent(StreamError{msg, static_cast<int>(httpStatus), acc.identifier});
    onEvent(StreamDone{StopReason::Error});
    return;
  }
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
}

std::string KiroProvider::buildCodeWhispererRequest(
    const AgentHistory &history,
    const std::string &modelId,
    const OAuthAccount &acc,
    const ProviderOptions &opts) {

  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();

  std::string conversationId = generateUuid();
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

  // Add tools if provided
  if (!opts.tools.empty()) {
    rapidjson::Value ctx(rapidjson::kObjectType);
    rapidjson::Value toolsArr(rapidjson::kArrayType);
    
    for (const auto &tool : opts.tools) {
      rapidjson::Value toolSpec(rapidjson::kObjectType);
      rapidjson::Value ts(rapidjson::kObjectType);
      ts.AddMember("name", rapidjson::Value(tool.name.c_str(), alloc), alloc);
      ts.AddMember("description", rapidjson::Value(tool.description.c_str(), alloc), alloc);
      
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
        "{\"thinking\":",
        "{\"reasoning\":",
        "{\"name\":",
        "{\"input\":",
        "{\"stop\":",
        "{\"contextUsagePercentage\":",
        "{\"followupPrompt\":",
        "{",
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
