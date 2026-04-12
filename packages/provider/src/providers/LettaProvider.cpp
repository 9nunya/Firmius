#include "providers/LettaProvider.hpp"
#include "utils/GCPHttpClient.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtil.hpp"
#include "utils/TempOAuthServer.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <pwd.h>
#include <sstream>
#include <thread>

namespace firmius::provider {

namespace {

constexpr char kProviderId[] = "letta";
constexpr char kClientId[] = "ci-let-724dea7e98f4af6f8f370f4b1466200c";
constexpr char kAuthBaseUrl[] = "https://app.letta.com";
constexpr char kApiBaseUrl[] = "https://api.letta.com";
constexpr char kSourceHeaderValue[] = "letta-code";
constexpr char kUserAgentValue[] = "letta-code/0.21.5";
constexpr char kDeviceCodeEndpoint[] = "/api/oauth/device/code";
constexpr char kTokenEndpoint[] = "/api/oauth/token";
constexpr char kModelsEndpoint[] = "/v1/models";
constexpr char kBalanceEndpoint[] = "/v1/metadata/balance";
constexpr int kQuotaRefreshSeconds = 300;
constexpr float kQuotaAvailableThreshold = 0.01f;

std::mutex gRawSseLogMutex;

[[maybe_unused]] void appendRawSseLog(const char *kind,
                                      std::string_view payload) {
  const char *path = std::getenv("FIRMIUS_LETTA_RAW_SSE_LOG");
  const char *stdoutFlag = std::getenv("FIRMIUS_LETTA_RAW_SSE_STDOUT");
  if ((!path || std::string_view(path).empty()) &&
      (!stdoutFlag || std::string_view(stdoutFlag).empty())) {
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
          .count();

  std::lock_guard<std::mutex> lock(gRawSseLogMutex);
  if (path && !std::string_view(path).empty()) {
    std::ofstream out(path, std::ios::app);
    if (out.is_open()) {
      out << "[" << ms << "] [" << kind << "] ";
      out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
      if (payload.empty() || payload.back() != '\n') {
        out << '\n';
      }
    }
  }
  if (stdoutFlag && std::string_view(stdoutFlag).size() > 0 &&
      std::string_view(stdoutFlag) != "0") {
    std::cout << "[LETTA_RAW_SSE " << kind << "] ";
    std::cout.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (payload.empty() || payload.back() != '\n') {
      std::cout << '\n';
    }
    std::cout << std::flush;
  }
}

// Stable device identifier for OAuth device code flow
// Letta's OAuth server requires device_id to correlate polling with registration
std::string getDeviceId() {
  // Use hostname + static salt for a stable per-machine identifier
  char hostname[256] = {};
  if (gethostname(hostname, sizeof(hostname)) == 0) {
    return std::string("firmius-") + hostname;
  }
  return "firmius-unknown";
}

int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::optional<OAuthAccount> loadLettaCliSettingsAccount() {
  std::string home;
  if (const char *homeEnv = std::getenv("HOME");
      homeEnv && std::string_view(homeEnv).size() > 0) {
    home = homeEnv;
  } else {
    if (const passwd *pw = getpwuid(getuid()); pw && pw->pw_dir) {
      home = pw->pw_dir;
    }
  }
  if (home.empty()) {
    return std::nullopt;
  }

  const std::filesystem::path settingsPath =
      std::filesystem::path(home) / ".letta" / "settings.json";
  if (!std::filesystem::exists(settingsPath)) {
    return std::nullopt;
  }

  std::ifstream ifs(settingsPath);
  if (!ifs.is_open()) {
    return std::nullopt;
  }

  const std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
  rapidjson::Document doc;
  doc.Parse(content.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return std::nullopt;
  }

  std::string accessToken;
  if (doc.HasMember("env") && doc["env"].IsObject()) {
    const auto &env = doc["env"];
    if (env.HasMember("LETTA_API_KEY") && env["LETTA_API_KEY"].IsString()) {
      accessToken = env["LETTA_API_KEY"].GetString();
    }
  }
  if (accessToken.empty()) {
    return std::nullopt;
  }

  OAuthAccount acc;
  acc.identifier = "letta-cli-settings";
  acc.accessToken = accessToken;
  if (doc.HasMember("refreshToken") && doc["refreshToken"].IsString()) {
    acc.refreshToken = doc["refreshToken"].GetString();
  }

  const int64_t now = nowSeconds();
  acc.tokenExpiration = now + 86400 * 365;
  if (doc.HasMember("tokenExpiresAt") && doc["tokenExpiresAt"].IsInt64()) {
    const int64_t expiresAt = doc["tokenExpiresAt"].GetInt64();
    if (expiresAt > now + 60) {
      acc.tokenExpiration = expiresAt;
    }
  }

  if (doc.HasMember("lastSession") && doc["lastSession"].IsObject()) {
    const auto &lastSession = doc["lastSession"];
    if (lastSession.HasMember("agentId") && lastSession["agentId"].IsString()) {
      acc.metadata["agent_id"] = lastSession["agentId"].GetString();
    }
  }
  acc.metadata["auth_source"] = "letta_settings";
  acc.metadata["ephemeral"] = "1";
  return acc;
}

[[maybe_unused]] bool isAutoAliasModel(const std::string &modelId) {
  return modelId.empty() || modelId == "auto" || modelId == "auto-fast" ||
         modelId == "auto-chat";
}

[[maybe_unused]] StopReason mapLettaStopReason(const std::string &reason) {
  if (reason == "tool_use" || reason == "tool_calls" || reason == "tool_call") {
    return StopReason::ToolUse;
  }
  if (reason == "max_tokens" || reason == "length") {
    return StopReason::MaxTokens;
  }
  if (reason == "content_filter") {
    return StopReason::ContentFilter;
  }
  if (reason == "cancelled") {
    return StopReason::Cancelled;
  }
  return StopReason::Stop;
}

[[maybe_unused]] bool
parseBalanceValue(const std::map<std::string, std::string> &metadata,
                  double &outBalance) {
  auto parseField = [&](const char *key, double &out) -> bool {
    auto it = metadata.find(key);
    if (it == metadata.end() || it->second.empty()) {
      return false;
    }
    try {
      out = std::stod(it->second);
      return true;
    } catch (...) {
      return false;
    }
  };

  double total = 0.0;
  if (parseField("total_balance", total)) {
    outBalance = total;
    return true;
  }

  double monthly = 0.0;
  double purchased = 0.0;
  const bool hasMonthly = parseField("monthly_credit_balance", monthly);
  const bool hasPurchased = parseField("purchased_credit_balance", purchased);
  if (hasMonthly || hasPurchased) {
    outBalance = monthly + purchased;
    return true;
  }

  return false;
}

[[maybe_unused]] bool shouldTreatAsStaleAgentFailure(int statusCode,
                                                     const std::string &body) {
  if (statusCode != 400 && statusCode != 401 && statusCode != 403 &&
      statusCode != 404) {
    return false;
  }
  std::string lowered = body;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return lowered.find("agent") != std::string::npos &&
         (lowered.find("not found") != std::string::npos ||
          lowered.find("invalid") != std::string::npos ||
          lowered.find("unauthorized") != std::string::npos ||
          lowered.find("forbidden") != std::string::npos ||
          lowered.find("permission") != std::string::npos);
}

std::string roleToLettaRole(firmius::shared::Role role) {
  switch (role) {
  case firmius::shared::Role::System:
    return "system";
  case firmius::shared::Role::User:
    return "user";
  case firmius::shared::Role::Assistant:
    return "assistant";
  case firmius::shared::Role::ToolResult:
    return "tool";
  case firmius::shared::Role::Error:
    return "system";
  }
  return "user";
}

bool parseDataUrl(const std::string &url, std::string &mediaType,
                  std::string &data) {
  constexpr std::string_view prefix = "data:";
  if (url.rfind(prefix.data(), 0) != 0) {
    return false;
  }

  const size_t commaPos = url.find(',');
  if (commaPos == std::string::npos || commaPos <= prefix.size()) {
    return false;
  }

  const std::string header = url.substr(prefix.size(), commaPos - prefix.size());
  const size_t semicolonPos = header.find(';');
  mediaType = semicolonPos == std::string::npos ? header
                                                : header.substr(0, semicolonPos);
  if (mediaType.empty()) {
    mediaType = "image/png";
  }
  data = url.substr(commaPos + 1);
  return !data.empty();
}

rapidjson::Value makeTextPart(const std::string &text,
                              rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value part(rapidjson::kObjectType);
  part.AddMember("type", "text", alloc);
  part.AddMember("text", rapidjson::Value(text.c_str(), alloc), alloc);
  return part;
}

rapidjson::Value makeImagePart(const firmius::shared::ImageContent &img,
                               rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value part(rapidjson::kObjectType);
  part.AddMember("type", "image", alloc);

  rapidjson::Value source(rapidjson::kObjectType);
  std::string mediaType;
  std::string data;
  if (parseDataUrl(img.url, mediaType, data)) {
    source.AddMember("type", "base64", alloc);
    source.AddMember("mediaType", rapidjson::Value(mediaType.c_str(), alloc),
                     alloc);
    source.AddMember("data", rapidjson::Value(data.c_str(), alloc), alloc);
  } else {
    source.AddMember("type", "url", alloc);
    source.AddMember("url", rapidjson::Value(img.url.c_str(), alloc), alloc);
    if (!img.mediaType.empty()) {
      source.AddMember("mediaType",
                       rapidjson::Value(img.mediaType.c_str(), alloc), alloc);
    }
  }

  part.AddMember("source", source, alloc);
  return part;
}

rapidjson::Value makeToolReturnValue(
    const firmius::shared::ToolResultContent &result,
    rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value value(rapidjson::kArrayType);
  value.PushBack(makeTextPart(result.result, alloc), alloc);
  return value;
}

rapidjson::Value makeApprovalMessage(
    const firmius::shared::Message &msg,
    rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value approvalMessage(rapidjson::kObjectType);
  approvalMessage.AddMember("type", "approval", alloc);

  rapidjson::Value approvals(rapidjson::kArrayType);
  for (const auto &part : msg.content) {
    const auto *result =
        std::get_if<firmius::shared::ToolResultContent>(&part);
    if (!result || result->toolCallId.empty()) {
      continue;
    }

    rapidjson::Value approval(rapidjson::kObjectType);
    approval.AddMember("type", "tool", alloc);
    approval.AddMember("tool_call_id",
                       rapidjson::Value(result->toolCallId.c_str(), alloc),
                       alloc);
    approval.AddMember("tool_return", makeToolReturnValue(*result, alloc),
                       alloc);
    approval.AddMember(
        "status",
        rapidjson::Value(result->success ? "success" : "error", alloc), alloc);
    approvals.PushBack(approval, alloc);
  }

  approvalMessage.AddMember("approvals", approvals, alloc);
  if (!msg.id.empty()) {
    approvalMessage.AddMember("otid", rapidjson::Value(msg.id.c_str(), alloc),
                              alloc);
  }
  return approvalMessage;
}

bool buildLettaMessage(const firmius::shared::Message &msg,
                       rapidjson::Value &outMessage,
                       rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value content(rapidjson::kArrayType);
  std::string flattenedText;
  bool hasStructuredPart = false;

  for (const auto &part : msg.content) {
    if (const auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
      if (!txt->text.empty()) {
        content.PushBack(makeTextPart(txt->text, alloc), alloc);
        flattenedText += txt->text;
      }
    } else if (const auto *img =
                   std::get_if<firmius::shared::ImageContent>(&part)) {
      content.PushBack(makeImagePart(*img, alloc), alloc);
      hasStructuredPart = true;
    } else if (const auto *thinking =
                   std::get_if<firmius::shared::ThinkingContent>(&part)) {
      if (!thinking->thinking.empty()) {
        const std::string wrapped =
            "<thinking>\n" + thinking->thinking + "\n</thinking>";
        content.PushBack(makeTextPart(wrapped, alloc), alloc);
        flattenedText += wrapped;
      }
    } else if (const auto *call =
                   std::get_if<firmius::shared::ToolCallContent>(&part)) {
      std::string rendered = "[Tool Call";
      if (!call->name.empty()) {
        rendered += ": " + call->name;
      }
      if (!call->args.empty()) {
        rendered += " args=" + call->args;
      }
      rendered += "]";
      content.PushBack(makeTextPart(rendered, alloc), alloc);
      flattenedText += rendered;
    }
  }

  if (content.Empty()) {
    return false;
  }

  outMessage.SetObject();
  outMessage.AddMember("role",
                       rapidjson::Value(roleToLettaRole(msg.role).c_str(),
                                        alloc),
                       alloc);
  if (!msg.id.empty()) {
    outMessage.AddMember("otid", rapidjson::Value(msg.id.c_str(), alloc),
                         alloc);
  }
  if (hasStructuredPart || content.Size() > 1) {
    outMessage.AddMember("content", content, alloc);
  } else {
    outMessage.AddMember("content",
                         rapidjson::Value(flattenedText.c_str(), alloc), alloc);
  }
  return true;
}

rapidjson::Value buildClientTools(
    const std::vector<firmius::provider::ToolDefinition> &tools,
    rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value clientTools(rapidjson::kArrayType);
  for (const auto &tool : tools) {
    rapidjson::Value toolValue(rapidjson::kObjectType);
    toolValue.AddMember("name", rapidjson::Value(tool.name.c_str(), alloc),
                        alloc);
    toolValue.AddMember(
        "description", rapidjson::Value(tool.description.c_str(), alloc),
        alloc);

    rapidjson::Document schemaDoc;
    schemaDoc.Parse(tool.inputSchema.c_str());
    rapidjson::Value parameters(rapidjson::kObjectType);
    if (!schemaDoc.HasParseError() && schemaDoc.IsObject()) {
      parameters.CopyFrom(schemaDoc, alloc);
    }
    toolValue.AddMember("parameters", parameters, alloc);
    clientTools.PushBack(toolValue, alloc);
  }
  return clientTools;
}
rapidjson::Value buildClientSkills(
    rapidjson::Document::AllocatorType &alloc) {
  (void)alloc;
  rapidjson::Value clientSkills(rapidjson::kArrayType);
  return clientSkills;
}

} // namespace

// ============================================================================
// OAuthWizard for Letta (Device Code Flow)
// ============================================================================

class LettaOAuthWizard : public OAuthWizard {
public:
  explicit LettaOAuthWizard(LettaProvider *provider) : provider_(provider) {
    // Step 1: Request device code
    firmius::utils::GCPHttpClient client("firmius-letta/1.0");
    client.setContentType("application/json");
    client.addHeader("X-Letta-Source", kSourceHeaderValue);
    client.addHeader("User-Agent", kUserAgentValue);
    std::string body =
        "{\"client_id\":\"" + std::string(kClientId) + "\"}";

    auto resp = client.post(std::string(kAuthBaseUrl) + kDeviceCodeEndpoint,
                            body, 15);

    if (resp.code != 200) {
      error_ = "Failed to request device code: HTTP " +
               std::to_string(resp.code) + " " + resp.body;
      finished_.store(true);
      return;
    }

    rapidjson::Document doc;
    doc.Parse(resp.body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
      error_ = "Invalid device code response";
      finished_.store(true);
      return;
    }

    if (doc.HasMember("device_code") && doc["device_code"].IsString())
      deviceCode_ = doc["device_code"].GetString();
    if (doc.HasMember("user_code") && doc["user_code"].IsString())
      userCode_ = doc["user_code"].GetString();
    if (doc.HasMember("verification_uri") &&
        doc["verification_uri"].IsString())
      verificationUri_ = doc["verification_uri"].GetString();
    if (doc.HasMember("verification_uri_complete") &&
        doc["verification_uri_complete"].IsString())
      verificationUriComplete_ =
          doc["verification_uri_complete"].GetString();
    if (doc.HasMember("expires_in") && doc["expires_in"].IsInt())
      expiresIn_ = doc["expires_in"].GetInt();
    if (doc.HasMember("interval") && doc["interval"].IsInt())
      interval_ = doc["interval"].GetInt();

    if (deviceCode_.empty() || userCode_.empty()) {
      error_ = "Missing device_code or user_code in response";
      finished_.store(true);
      return;
    }

    prompt_ = "Letta OAuth Setup\n\n"
              "1. Open this URL in your browser:\n"
              "   " +
              verificationUriComplete_ + "\n\n"
              "2. Enter this code: " +
              userCode_ + "\n\n"
              "3. Complete authorization in your browser. Firmius will keep "
              "polling automatically.";

    pollingThread_ = std::thread([this]() { pollForToken(); });
  }

  ~LettaOAuthWizard() override {
    if (pollingThread_.joinable()) {
      pollingThread_.join();
    }
  }

  std::optional<WizardPrompt> nextPrompt() override {
    if (!promptShown_) {
      promptShown_ = true;
      if (!error_.empty()) {
        return WizardPrompt{"Error: " + error_, false};
      }
      return WizardPrompt{prompt_, false};
    }
    return std::nullopt;
  }

  void submitAnswer(const std::string &) override {}

  bool isComplete() const override { return finished_.load(); }

  bool finalizeExchange(std::string &outErrorMessage) override {
    if (pollingThread_.joinable()) {
      pollingThread_.join();
    }

    if (!error_.empty()) {
      outErrorMessage = error_;
      return false;
    }

    if (!tokenReceived_.load() || accessToken_.empty() || refreshToken_.empty()) {
      outErrorMessage = "OAuth flow did not complete successfully";
      return false;
    }

    OAuthAccount acc;
    acc.accessToken = accessToken_;
    acc.refreshToken = refreshToken_;
    acc.tokenExpiration = tokenExpiration_;
    acc.identifier = userCode_; // temporary, will be updated on first use

    // Try to extract user info from the token
    if (!accessToken_.empty()) {
      std::string email = extractEmailFromJwt(accessToken_);
      if (!email.empty()) {
        acc.identifier = email;
      }
    }

    provider_->addAccount(acc);
    return true;
  }

  std::string getFinalMessage() const override {
    return "Successfully authenticated with Letta!";
  }

private:
  void pollForToken() {
    firmius::utils::GCPHttpClient client("firmius-letta/1.0");
    client.setContentType("application/json");
    client.addHeader("X-Letta-Source", kSourceHeaderValue);
    client.addHeader("User-Agent", kUserAgentValue);

    int64_t startTime = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    int pollInterval = interval_;
    int64_t expiresAt = startTime + expiresIn_;

    while (std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count() < expiresAt) {
      std::this_thread::sleep_for(std::chrono::seconds(pollInterval));

      std::string deviceId = getDeviceId();
      std::string deviceName = [&]() {
        char h[256] = {};
        gethostname(h, sizeof(h));
        return std::string(h);
      }();
      std::string body = "{\"grant_type\":\"urn:ietf:params:oauth:grant-type:"
                         "device_code\",";
      body += "\"client_id\":\"" + std::string(kClientId) + "\",";
      body += "\"device_code\":\"" + deviceCode_ + "\",";
      body += "\"device_id\":\"" + deviceId + "\",";
      body += "\"device_name\":\"" + deviceName + "\"}";

      auto resp = client.post(std::string(kAuthBaseUrl) + kTokenEndpoint,
                              body, 15);
      if (resp.code != 200) {
        rapidjson::Document errDoc;
        errDoc.Parse(resp.body.c_str());
        if (!errDoc.HasParseError() && errDoc.IsObject() &&
            errDoc.HasMember("error") && errDoc["error"].IsString()) {
          std::string err = errDoc["error"].GetString();
          if (err == "authorization_pending" || err == "slow_down") {
            if (err == "slow_down")
              pollInterval += 5;
            continue;
          }
          if (err == "access_denied") {
            error_ = "User denied authorization";
            finished_.store(true);
            return;
          }
          if (err == "expired_token") {
            error_ = "Device code expired";
            finished_.store(true);
            return;
          }
        }
        error_ = "Token poll failed: HTTP " + std::to_string(resp.code);
        finished_.store(true);
        return;
      }

      rapidjson::Document doc;
      doc.Parse(resp.body.c_str());
      if (doc.HasParseError() || !doc.IsObject()) {
        error_ = "Invalid token response";
        finished_.store(true);
        return;
      }

      if (doc.HasMember("access_token") && doc["access_token"].IsString())
        accessToken_ = doc["access_token"].GetString();
      if (doc.HasMember("refresh_token") && doc["refresh_token"].IsString())
        refreshToken_ = doc["refresh_token"].GetString();
      if (doc.HasMember("expires_in") && doc["expires_in"].IsInt()) {
        tokenExpiration_ =
            nowSeconds() + doc["expires_in"].GetInt();
      }

      if (!accessToken_.empty()) {
        tokenReceived_.store(true);
        finished_.store(true);
        return;
      }

      error_ = "No access_token in token response";
      finished_.store(true);
      return;
    }

    error_ = "OAuth polling timed out";
    finished_.store(true);
  }

  static std::string extractEmailFromJwt(const std::string &token) {
    size_t firstDot = token.find('.');
    if (firstDot == std::string::npos)
      return "";
    size_t secondDot = token.find('.', firstDot + 1);
    if (secondDot == std::string::npos)
      return "";

    std::string payload = token.substr(firstDot + 1, secondDot - firstDot - 1);
    switch (payload.size() % 4) {
    case 2:
      payload += "==";
      break;
    case 3:
      payload += "=";
      break;
    default:
      break;
    }

    static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string decoded;
    decoded.reserve(payload.size() * 3 / 4);
    int val = 0, valb = -8;
    for (char c : payload) {
      size_t pos = base64_chars.find(c);
      if (pos == std::string::npos)
        continue;
      val = (val << 6) + static_cast<int>(pos);
      valb += 6;
      if (valb >= 0) {
        decoded.push_back(static_cast<char>((val >> valb) & 0xFF));
        valb -= 8;
      }
    }

    rapidjson::Document d;
    d.Parse(decoded.c_str());
    if (!d.HasParseError() && d.IsObject()) {
      if (d.HasMember("email") && d["email"].IsString())
        return d["email"].GetString();
      if (d.HasMember("preferred_username") &&
          d["preferred_username"].IsString())
        return d["preferred_username"].GetString();
    }
    return "";
  }

  LettaProvider *provider_;
  std::thread pollingThread_;
  std::string prompt_;
  bool promptShown_ = false;
  std::atomic<bool> finished_{false};
  std::atomic<bool> tokenReceived_{false};
  std::string error_;

  std::string deviceCode_;
  std::string userCode_;
  std::string verificationUri_;
  std::string verificationUriComplete_;
  int expiresIn_ = 900;
  int interval_ = 5;

  std::string accessToken_;
  std::string refreshToken_;
  int64_t tokenExpiration_ = 0;
};

// ============================================================================
// LettaProvider Implementation
// ============================================================================

LettaProvider::LettaProvider() : BaseOAuthProvider(kProviderId) {}

LettaProvider::~LettaProvider() { stopBackgroundQuotaRefresh(); }

// ============================================================================
// Static Models (from Letta models.json)
// ============================================================================

std::map<std::string, ModelInfo> LettaProvider::getStaticModels() {
  return {
      {"auto",
       {.id = "auto",
        .provider = kProviderId,
        .contextWindow = 140000,
        .modalities = {"text"},
        .variants = {},
        .supportsReasoning = true}},
      {"auto-fast",
       {.id = "auto-fast",
        .provider = kProviderId,
        .contextWindow = 140000,
        .modalities = {"text"},
        .variants = {},
        .supportsReasoning = true}},
      {"auto-chat",
       {.id = "auto-chat",
        .provider = kProviderId,
        .contextWindow = 140000,
        .modalities = {"text"},
        .variants = {},
        .supportsReasoning = true}},
      {"sonnet",
       {.id = "sonnet",
        .provider = kProviderId,
        .contextWindow = 200000,
        .modalities = {"text", "image"},
        .variants = {},
        .supportsReasoning = true}},
      {"sonnet-1m",
       {.id = "sonnet-1m",
        .provider = kProviderId,
        .contextWindow = 1000000,
        .modalities = {"text", "image"},
        .variants = {},
        .supportsReasoning = true}},
      {"opus",
       {.id = "opus",
        .provider = kProviderId,
        .contextWindow = 200000,
        .modalities = {"text", "image"},
        .variants = {},
        .supportsReasoning = true}},
      {"gpt-5.2",
       {.id = "gpt-5.2",
        .provider = kProviderId,
        .contextWindow = 272000,
        .modalities = {"text", "image"},
        .variants = {},
        .supportsReasoning = true}},
      {"gpt-5.1",
       {.id = "gpt-5.1",
        .provider = kProviderId,
        .contextWindow = 128000,
        .modalities = {"text", "image"},
        .variants = {},
        .supportsReasoning = true}},
      {"gemini-2.5-pro",
       {.id = "gemini-2.5-pro",
        .provider = kProviderId,
        .contextWindow = 1000000,
        .modalities = {"text", "image"},
        .variants = {},
        .supportsReasoning = true}},
      {"kimi-k2",
       {.id = "kimi-k2",
        .provider = kProviderId,
        .contextWindow = 128000,
        .modalities = {"text"},
        .variants = {},
        .supportsReasoning = true}},
      {"glm-4.6",
       {.id = "glm-4.6",
        .provider = kProviderId,
        .contextWindow = 128000,
        .modalities = {"text"},
        .variants = {},
        .supportsReasoning = true}},
  };
}

std::vector<ModelInfo> LettaProvider::listModels() {
  std::vector<ModelInfo> result;
  for (const auto &[id, info] : getStaticModels())
    result.push_back(info);
  return result;
}

ModelInfo LettaProvider::getModelInfo(const std::string &modelId) {
  auto models = getStaticModels();
  if (models.count(modelId))
    return models[modelId];
  return {.id = modelId,
          .provider = kProviderId,
          .contextWindow = 128000,
          .modalities = {"text"},
          .variants = {},
          .supportsReasoning = true};
}

// ============================================================================
// OAuth
// ============================================================================

std::unique_ptr<OAuthWizard> LettaProvider::beginConnectionWizard() {
  return std::make_unique<LettaOAuthWizard>(this);
}

bool LettaProvider::refreshAccessToken(OAuthAccount &acc) {
  firmius::utils::GCPHttpClient client("firmius-letta/1.0");
  client.setContentType("application/json");
  client.addHeader("X-Letta-Source", kSourceHeaderValue);
  client.addHeader("User-Agent", kUserAgentValue);

  std::string deviceId = getDeviceId();
  std::string deviceName = [&]() {
    char h[256] = {};
    gethostname(h, sizeof(h));
    return std::string(h);
  }();
  std::string body = "{\"grant_type\":\"refresh_token\",";
  body += "\"client_id\":\"" + std::string(kClientId) + "\",";
  body += "\"refresh_token\":\"" + acc.refreshToken + "\",";
  body += "\"refresh_token_mode\":\"new\",";
  body += "\"device_id\":\"" + deviceId + "\",";
  body += "\"device_name\":\"" + deviceName + "\"}";

  auto resp =
      client.post(std::string(kAuthBaseUrl) + kTokenEndpoint, body, 15);
  if (resp.code == 200) {
    rapidjson::Document doc;
    doc.Parse(resp.body.c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      bool updated = false;
      if (doc.HasMember("access_token") && doc["access_token"].IsString()) {
        acc.accessToken = doc["access_token"].GetString();
        updated = true;
      }
      if (doc.HasMember("refresh_token") && doc["refresh_token"].IsString()) {
        acc.refreshToken = doc["refresh_token"].GetString();
      }
      if (doc.HasMember("expires_in") && doc["expires_in"].IsInt()) {
        acc.tokenExpiration = nowSeconds() + doc["expires_in"].GetInt();
      }
      if (updated) {
        saveAccounts();
        return true;
      }
    }
  }
  return false;
}

// ============================================================================
// Quota
// ============================================================================

bool LettaProvider::fetchAndStoreQuotas(OAuthAccount &acc) {
  if (acc.accessToken.empty())
    return false;

  firmius::utils::GCPHttpClient client("firmius-letta/1.0");
  client.addHeader("X-Letta-Source", kSourceHeaderValue);
  client.addHeader("User-Agent", kUserAgentValue);
  client.addHeader("Authorization", "Bearer " + acc.accessToken);

  auto resp = client.get(std::string(kApiBaseUrl) + kBalanceEndpoint, 10);
  if (resp.code != 200)
    return false;

  rapidjson::Document doc;
  doc.Parse(resp.body.c_str());
  if (doc.HasParseError() || !doc.IsObject())
    return false;

  int64_t now = nowSeconds();
  acc.lastQuotaRefresh = now;

  if (doc.HasMember("billing_tier") && doc["billing_tier"].IsString()) {
    acc.metadata["billing_tier"] = doc["billing_tier"].GetString();
  }
  if (doc.HasMember("total_balance") && doc["total_balance"].IsNumber()) {
    acc.metadata["total_balance"] =
        std::to_string(doc["total_balance"].GetDouble());
  }
  if (doc.HasMember("monthly_credit_balance") &&
      doc["monthly_credit_balance"].IsNumber()) {
    acc.metadata["monthly_credit_balance"] =
        std::to_string(doc["monthly_credit_balance"].GetDouble());
  }
  if (doc.HasMember("purchased_credit_balance") &&
      doc["purchased_credit_balance"].IsNumber()) {
    acc.metadata["purchased_credit_balance"] =
        std::to_string(doc["purchased_credit_balance"].GetDouble());
  }

  saveAccounts();
  return true;
}

void LettaProvider::refreshQuotas() {
  if (accounts_.empty())
    return;

  int64_t now = nowSeconds();
  bool needsSave = false;

  for (auto &acc : accounts_) {
    if (isTokenExpired(acc)) {
      if (!refreshAccessToken(acc)) {
        continue;
      }
    }

    if (acc.lastQuotaRefresh == 0 ||
        (now - acc.lastQuotaRefresh) > kQuotaRefreshSeconds) {
      fetchAndStoreQuotas(acc);
      needsSave = true;
    }

    if (acc.rateLimited && now > acc.backoffUntil) {
      acc.rateLimited = false;
      needsSave = true;
    }
  }

  if (needsSave) {
    saveAccounts();
  }
}

std::map<std::string, std::vector<QuotaBucket>>
LettaProvider::getAllQuotas() const {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  std::map<std::string, std::vector<QuotaBucket>> result;

  for (const auto &acc : accounts_) {
    std::vector<QuotaBucket> buckets;

    auto balIt = acc.metadata.find("total_balance");
    if (balIt != acc.metadata.end()) {
      QuotaBucket bucket;
      bucket.name = "balance";
      bucket.remainingFraction = 1.0f;
      bucket.note = "Balance: $" + balIt->second;

      auto tierIt = acc.metadata.find("billing_tier");
      if (tierIt != acc.metadata.end()) {
        bucket.note += " (Tier: " + tierIt->second + ")";
      }
      buckets.push_back(bucket);
    }

    auto mcIt = acc.metadata.find("monthly_credit_balance");
    if (mcIt != acc.metadata.end()) {
      QuotaBucket bucket;
      bucket.name = "monthly_credit";
      bucket.remainingFraction = 1.0f;
      bucket.note = "Monthly credit: $" + mcIt->second;
      buckets.push_back(bucket);
    }

    if (!buckets.empty()) {
      result[acc.identifier] = std::move(buckets);
    }
  }

  return result;
}

std::optional<OAuthAccount>
LettaProvider::getAvailableAccount(const std::optional<std::string> & /*modelId*/) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);

  auto tryCliFallback = [&]() -> std::optional<OAuthAccount> {
    return loadLettaCliSettingsAccount();
  };

  if (auto cliAcc = tryCliFallback()) {
    return cliAcc;
  }

  if (accounts_.empty()) {
    return std::nullopt;
  }

  const int64_t now = nowSeconds();
  const int accountCount = static_cast<int>(accounts_.size());
  int startIdx = lastUsedIndex_.load(std::memory_order_relaxed);
  if (startIdx < 0 || startIdx >= accountCount) {
    startIdx = 0;
  } else {
    startIdx = (startIdx + 1) % accountCount;
  }

  int selectedIdx = -1;
  bool stateChanged = false;

  for (int offset = 0; offset < accountCount; ++offset) {
    const int idx = (startIdx + offset) % accountCount;
    OAuthAccount &acc = accounts_[idx];

    if (acc.rateLimited && now > acc.backoffUntil) {
      acc.rateLimited = false;
      stateChanged = true;
    }
    if (acc.rateLimited) {
      continue;
    }

    if (isTokenExpired(acc) && !refreshAccessToken(acc)) {
      markAccountRateLimited(acc, 60);
      stateChanged = true;
      continue;
    }

    if (acc.lastQuotaRefresh == 0 ||
        (now - acc.lastQuotaRefresh) > kQuotaRefreshSeconds) {
      fetchAndStoreQuotas(acc);
    }

    selectedIdx = idx;
    break;
  }

  if (selectedIdx < 0) {
    if (stateChanged) {
      saveAccounts();
    }
    return tryCliFallback();
  }

  if (lastUsedIndex_.load(std::memory_order_relaxed) != selectedIdx) {
    lastUsedIndex_.store(selectedIdx, std::memory_order_relaxed);
    stateChanged = true;
  }
  if (stateChanged) {
    saveAccounts();
  }
  return accounts_[selectedIdx];
}

// ============================================================================
// SSE Streaming
// ============================================================================

size_t LettaProvider::sseWriteCallback(char *ptr, size_t size, size_t nmemb,
                                       void *userdata) {
  auto *ctx = static_cast<StreamContext *>(userdata);
  size_t total = size * nmemb;

  ctx->buffer.append(ptr, total);

  while (true) {
    size_t newlinePos = ctx->buffer.find('\n', ctx->readOffset);
    if (newlinePos == std::string::npos)
      break;

    std::string line = ctx->buffer.substr(ctx->readOffset,
                                          newlinePos - ctx->readOffset);
    ctx->readOffset = newlinePos + 1;

    if (ctx->abortSignal && ctx->abortSignal->load()) {
      return 0;
    }

    ctx->provider->processSSELine(line, *ctx);
  }

  return total;
}

void LettaProvider::processSSELine(const std::string &line,
                                   StreamContext &ctx) {
  if (line.empty() || line[0] == ':')
    return;

  std::string data;
  if (line.rfind("data: ", 0) == 0) {
    data = line.substr(6);
  } else if (line.rfind("data:", 0) == 0) {
    data = line.substr(5);
  } else {
    return;
  }

  data = StringUtil::trim(data);
  if (data.empty() || data == "[DONE]")
    return;

  appendRawSseLog("line", line);

  rapidjson::Document d;
  d.Parse(data.c_str());
  if (d.HasParseError() || !d.IsObject())
    return;

  auto valueToString = [](const rapidjson::Value &value) {
    if (value.IsString()) {
      return std::string(value.GetString());
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    value.Accept(writer);
    return std::string(sb.GetString());
  };

  std::string msgType;
  if (d.HasMember("message_type") && d["message_type"].IsString()) {
    msgType = d["message_type"].GetString();
  } else if (d.HasMember("type") && d["type"].IsString()) {
    msgType = d["type"].GetString();
  }

  if (msgType == "ping") {
    return;
  }

  auto emitError = [&](const std::string &message) {
    if (message.empty()) {
      return;
    }
    ctx.sawTypedEvent = true;
    ctx.sawProtocolError = true;
    StreamError se;
    se.message = message;
    (*ctx.onEvent)(se);
  };

  auto parseToolCallFields = [&](const rapidjson::Value &toolCall,
                                 std::string &toolCallId,
                                 std::string &toolName,
                                 std::string &args) {
    if (toolCall.HasMember("tool_call_id") &&
        toolCall["tool_call_id"].IsString()) {
      toolCallId = toolCall["tool_call_id"].GetString();
    } else if (toolCall.HasMember("tool_callId") &&
               toolCall["tool_callId"].IsString()) {
      toolCallId = toolCall["tool_callId"].GetString();
    } else if (toolCall.HasMember("id") && toolCall["id"].IsString()) {
      toolCallId = toolCall["id"].GetString();
    }
    if (toolCall.HasMember("name") && toolCall["name"].IsString()) {
      toolName = toolCall["name"].GetString();
    } else if (toolCall.HasMember("tool_name") &&
               toolCall["tool_name"].IsString()) {
      toolName = toolCall["tool_name"].GetString();
    }
    if (toolCall.HasMember("arguments")) {
      args = valueToString(toolCall["arguments"]);
    } else if (toolCall.HasMember("args") && toolCall["args"].IsString()) {
      args = toolCall["args"].GetString();
    } else if (toolCall.HasMember("tool_args") &&
               toolCall["tool_args"].IsString()) {
      args = toolCall["tool_args"].GetString();
    }
  };

  auto emitToolCallChunk = [&](const std::string &toolCallId,
                               const std::string &toolName,
                               const std::string &args) {
    if (toolCallId.empty()) {
      return;
    }
    auto &state = ctx.streamedToolCalls[toolCallId];
    if (!state.hasIndex) {
      state.index = ctx.toolCallCounter++;
      state.hasIndex = true;
      state.emittedId = toolCallId;
    }
    if (!toolName.empty()) {
      state.lastName = toolName;
    }
    if (!args.empty()) {
      state.lastArgs += args;
    }
    ctx.sawTypedEvent = true;
    ToolCallChunk tc;
    tc.id = toolCallId;
    tc.index = state.index;
    tc.nameDelta = toolName;
    tc.argsDelta = args;
    (*ctx.onEvent)(tc);
  };

  auto handleToolCallPayload = [&](const rapidjson::Value &payload,
                                   bool emitChunk) {
    std::string toolCallId;
    std::string toolName;
    std::string args;
    parseToolCallFields(payload, toolCallId, toolName, args);
    if (toolCallId.empty()) {
      return;
    }
    if (emitChunk) {
      emitToolCallChunk(toolCallId, toolName, args);
    } else {
      ctx.sawTypedEvent = true;
      ctx.streamedToolCalls.erase(toolCallId);
    }
  };

  if ((d.HasMember("error_message") && d["error_message"].IsString()) ||
      msgType == "error_message") {
    const std::string message = d.HasMember("error_message") &&
                                        d["error_message"].IsString()
                                    ? d["error_message"].GetString()
                                    : (d.HasMember("message") &&
                                               d["message"].IsString()
                                           ? d["message"].GetString()
                                           : "Letta stream error");
    emitError(message);
    return;
  }

  if (msgType == "usage_statistics") {
    ctx.sawTypedEvent = true;
    AgentMetrics metrics;
    if (d.HasMember("prompt_tokens") && d["prompt_tokens"].IsInt()) {
      metrics.tokens.prompt = d["prompt_tokens"].GetInt();
      metrics.tokens.contextSize = metrics.tokens.prompt;
    }
    if (d.HasMember("completion_tokens") && d["completion_tokens"].IsInt()) {
      metrics.tokens.completion = d["completion_tokens"].GetInt();
    }
    if (d.HasMember("total_tokens") && d["total_tokens"].IsInt()) {
      metrics.tokens.total = d["total_tokens"].GetInt();
    }
    if (d.HasMember("cached_input_tokens") &&
        d["cached_input_tokens"].IsInt()) {
      metrics.tokens.cacheRead = d["cached_input_tokens"].GetInt();
    }
    if (d.HasMember("reasoning_tokens") && d["reasoning_tokens"].IsInt()) {
      metrics.tokens.reasoning = d["reasoning_tokens"].GetInt();
    }
    (*ctx.onEvent)(metrics);
    return;
  }

  if (msgType == "assistant_message") {
    ctx.sawTypedEvent = true;
    std::string delta;
    if (d.HasMember("content")) {
      if (d["content"].IsString()) {
        delta = d["content"].GetString();
      } else if (d["content"].IsArray()) {
        for (rapidjson::SizeType i = 0; i < d["content"].Size(); ++i) {
          const auto &part = d["content"][i];
          if (part.IsString()) {
            delta += part.GetString();
          } else if (part.IsObject() && part.HasMember("text") &&
                     part["text"].IsString()) {
            delta += part["text"].GetString();
          }
        }
      } else if (d["content"].IsObject()) {
        if (d["content"].HasMember("text") &&
            d["content"]["text"].IsString()) {
          delta = d["content"]["text"].GetString();
        }
      }
    }
    if (!delta.empty()) {
      TextChunk tc;
      tc.delta = delta;
      (*ctx.onEvent)(tc);
    }
    return;
  }

  if (msgType == "reasoning_message") {
    ctx.sawTypedEvent = true;
    std::string reason;
    if (d.HasMember("reasoning") && d["reasoning"].IsString()) {
      reason = d["reasoning"].GetString();
    } else if (d.HasMember("content") && d["content"].IsString()) {
      reason = d["content"].GetString();
    }
    if (!reason.empty()) {
      ThinkingChunk rc;
      rc.delta = reason;
      (*ctx.onEvent)(rc);
    }
    return;
  }

  if (msgType == "approval_request_message") {
    if (d.HasMember("tool_call") && d["tool_call"].IsObject()) {
      handleToolCallPayload(d["tool_call"], true);
    } else if (d.HasMember("tool_calls") && d["tool_calls"].IsArray()) {
      ctx.sawTypedEvent = true;
      for (const auto &toolCall : d["tool_calls"].GetArray()) {
        if (toolCall.IsObject()) {
          handleToolCallPayload(toolCall, true);
        }
      }
    } else if (d.HasMember("approval_request") &&
               d["approval_request"].IsObject()) {
      handleToolCallPayload(d["approval_request"], true);
    }
    return;
  }

  if (msgType == "tool_call_message") {
    if (d.HasMember("tool_call") && d["tool_call"].IsObject()) {
      handleToolCallPayload(d["tool_call"], true);
    } else if (d.HasMember("tool_calls") && d["tool_calls"].IsArray()) {
      ctx.sawTypedEvent = true;
      for (const auto &toolCall : d["tool_calls"].GetArray()) {
        if (toolCall.IsObject()) {
          handleToolCallPayload(toolCall, true);
        }
      }
    }
    return;
  }

  if (msgType == "tool_return_message") {
    ctx.sawTypedEvent = true;
    auto clearToolCallState = [&](const std::string &toolCallId) {
      if (!toolCallId.empty()) {
        ctx.streamedToolCalls.erase(toolCallId);
      }
    };
    if (d.HasMember("tool_call_id") && d["tool_call_id"].IsString()) {
      clearToolCallState(d["tool_call_id"].GetString());
    } else if (d.HasMember("tool_call") && d["tool_call"].IsObject()) {
      std::string toolCallId;
      std::string toolName;
      std::string args;
      parseToolCallFields(d["tool_call"], toolCallId, toolName, args);
      clearToolCallState(toolCallId);
    } else if (d.HasMember("tool_returns") && d["tool_returns"].IsArray()) {
      for (const auto &toolReturn : d["tool_returns"].GetArray()) {
        if (!toolReturn.IsObject()) {
          continue;
        }
        std::string toolCallId;
        std::string toolName;
        std::string args;
        parseToolCallFields(toolReturn, toolCallId, toolName, args);
        clearToolCallState(toolCallId);
      }
    }
    return;
  }

  if (msgType == "stop_reason") {
    ctx.sawTypedEvent = true;
    if (d.HasMember("stop_reason") && d["stop_reason"].IsString()) {
      ctx.stopReason = mapLettaStopReason(d["stop_reason"].GetString());
      ctx.sawStopReason = true;
    } else if (d.HasMember("reason") && d["reason"].IsString()) {
      ctx.stopReason = mapLettaStopReason(d["reason"].GetString());
      ctx.sawStopReason = true;
    }
    return;
  }

  if (msgType == "api_error" || d.HasMember("error")) {
    ctx.sawTypedEvent = true;
    std::string errMsg;
    if (d.HasMember("error") && d["error"].IsObject()) {
      const auto &err = d["error"];
      if (err.HasMember("message") && err["message"].IsString()) {
        errMsg = err["message"].GetString();
      } else if (err.HasMember("error") && err["error"].IsString()) {
        errMsg = err["error"].GetString();
      } else {
        errMsg = valueToString(err);
      }
    } else if (d.HasMember("error") && d["error"].IsString()) {
      errMsg = d["error"].GetString();
    } else if (d.HasMember("message") && d["message"].IsString()) {
      errMsg = d["message"].GetString();
    } else if (d.HasMember("error_message") && d["error_message"].IsString()) {
      errMsg = d["error_message"].GetString();
    }
    emitError(errMsg);
    return;
  }
}

// ============================================================================
// Stream Execution
// ============================================================================

int LettaProvider::executeStreamRequest(
    OAuthAccount &acc, const AgentHistory &history, const ProviderOptions &opts,
    std::function<void(const StreamEvent &)> &onEvent) {

  std::string agentError;
  if (!ensureAgentId(acc, agentError)) {
    onEvent(StreamError{agentError, 0, acc.identifier});
    return 0;
  }

  std::string conversationId;
  std::string conversationError;
  if (!ensureConversationId(acc, conversationId, conversationError)) {
    onEvent(StreamError{conversationError, 0, acc.identifier});
    return 0;
  }

  CURL *curl = curl_easy_init();
  if (!curl)
    return 0;

  // Letta uses conversations API: POST /v1/conversations/{id}/messages
  std::string url = std::string(kApiBaseUrl) + "/v1/conversations/" +
                    conversationId + "/messages";

  rapidjson::Document doc(rapidjson::kObjectType);
  rapidjson::Document::AllocatorType &alloc = doc.GetAllocator();

  rapidjson::Value messages(rapidjson::kArrayType);

  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == firmius::shared::Role::Error) {
        continue;
      }

      if (msg.role == firmius::shared::Role::ToolResult) {
        rapidjson::Value approvalMessage = makeApprovalMessage(msg, alloc);
        if (approvalMessage.HasMember("approvals") &&
            approvalMessage["approvals"].IsArray() &&
            !approvalMessage["approvals"].Empty()) {
          messages.PushBack(approvalMessage, alloc);
        }
        continue;
      }

      rapidjson::Value lettaMsg;
      if (!buildLettaMessage(msg, lettaMsg, alloc)) {
        continue;
      }
      messages.PushBack(lettaMsg, alloc);
    }
  }

  doc.AddMember("messages", messages, alloc);
  doc.AddMember("streaming", true, alloc);
  doc.AddMember("stream_tokens", true, alloc);
  doc.AddMember("include_pings", true, alloc);
  doc.AddMember("background", true, alloc);
  doc.AddMember("client_skills", buildClientSkills(alloc), alloc);
  doc.AddMember("client_tools", buildClientTools(opts.tools, alloc), alloc);
  doc.AddMember("include_compaction_messages", true, alloc);

  const std::string agentId = acc.metadata["agent_id"];
  if (!agentId.empty()) {
    doc.AddMember("agent_id", rapidjson::Value(agentId.c_str(), alloc), alloc);
  }

  if (!opts.modelId.empty() && opts.modelId != "auto") {
    doc.AddMember("override_model",
                  rapidjson::Value(opts.modelId.c_str(), alloc), alloc);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  std::string body = buffer.GetString();

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(
      headers, ("Authorization: Bearer " + acc.accessToken).c_str());
  headers = curl_slist_append(headers, "Accept: text/event-stream");
  headers = curl_slist_append(headers, "X-Letta-Source: letta-code");
  headers = curl_slist_append(headers, "User-Agent: letta-code/0.21.5");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sseWriteCallback);

  StreamContext ctx;
  ctx.provider = this;
  ctx.onEvent = &onEvent;
  ctx.abortSignal = opts.abortSignal;

  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

  CURLcode res = curl_easy_perform(curl);
  long responseCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK && res != CURLE_ABORTED_BY_CALLBACK) {
    StreamError se;
    se.message = std::string("curl error: ") + curl_easy_strerror(res);
    onEvent(se);
    return 0;
  }

  if (responseCode >= 400) {
    StreamError se;
    se.httpStatus = static_cast<int>(responseCode);
    se.accountLocator = acc.identifier;
    se.message = "Letta API error";
    if (!ctx.buffer.empty()) {
      se.message += ": " + ctx.buffer;
    }
    onEvent(se);
    return static_cast<int>(responseCode);
  }

  if (!ctx.sawTypedEvent) {
    StreamError se;
    se.httpStatus = static_cast<int>(responseCode);
    se.accountLocator = acc.identifier;
    se.message = "Letta stream produced no meaningful events";
    onEvent(se);
    return static_cast<int>(responseCode);
  }

  if (!ctx.sawProtocolError) {
    onEvent(StreamDone{ctx.sawStopReason ? ctx.stopReason : StopReason::Stop});
  }
  return static_cast<int>(responseCode);
}
bool LettaProvider::ensureAgentId(OAuthAccount &acc,
                                  std::string &outErrorMessage) {
  const auto existing = acc.metadata.find("agent_id");
  if (existing != acc.metadata.end() && !existing->second.empty()) {
    return true;
  }

  firmius::utils::GCPHttpClient client("firmius-letta/1.0");
  client.setContentType("application/json");
  client.addHeader("X-Letta-Source", kSourceHeaderValue);
  client.addHeader("User-Agent", kUserAgentValue);
  client.addHeader("Authorization", "Bearer " + acc.accessToken);

  auto listResp = client.get(std::string(kApiBaseUrl) + "/v1/agents", 15);
  if (listResp.code == 200) {
    rapidjson::Document listDoc;
    listDoc.Parse(listResp.body.c_str());
    if (!listDoc.HasParseError() && listDoc.IsArray() && !listDoc.Empty()) {
      const auto &firstAgent = listDoc[0];
      if (firstAgent.IsObject() && firstAgent.HasMember("id") &&
          firstAgent["id"].IsString()) {
        acc.metadata["agent_id"] = firstAgent["id"].GetString();
        saveAccounts();
        return true;
      }
    }
  }

  auto resp = client.post(std::string(kApiBaseUrl) + "/v1/agents", "{}", 15);
  if (resp.code != 201 && resp.code != 200) {
    outErrorMessage = "Failed to create Letta agent: HTTP " +
                      std::to_string(resp.code);
    if (!resp.body.empty()) {
      outErrorMessage += " " + resp.body;
    }
    return false;
  }

  rapidjson::Document doc;
  doc.Parse(resp.body.c_str());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("id") ||
      !doc["id"].IsString()) {
    outErrorMessage = "Invalid Letta agent creation response";
    return false;
  }

  acc.metadata["agent_id"] = doc["id"].GetString();
  saveAccounts();
  return true;
}

std::string urlEncode(const std::string &value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex << std::uppercase;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      escaped << static_cast<char>(c);
    } else {
      escaped << '%' << std::setw(2) << static_cast<int>(c);
    }
  }
  return escaped.str();
}

bool LettaProvider::ensureConversationId(OAuthAccount &acc,
                                         std::string &outConversationId,
                                         std::string &outErrorMessage) {
  auto existing = acc.metadata.find("conversation_id");
  if (existing != acc.metadata.end() && !existing->second.empty() &&
      existing->second != "default") {
    outConversationId = existing->second;
    return true;
  }

  const auto agentIt = acc.metadata.find("agent_id");
  if (agentIt == acc.metadata.end() || agentIt->second.empty()) {
    outErrorMessage = "Missing Letta agent_id for conversation creation";
    return false;
  }

  firmius::utils::GCPHttpClient client("firmius-letta/1.0");
  client.setContentType("application/json");
  client.addHeader("X-Letta-Source", kSourceHeaderValue);
  client.addHeader("User-Agent", kUserAgentValue);
  client.addHeader("Authorization", "Bearer " + acc.accessToken);

  const std::string url = std::string(kApiBaseUrl) + "/v1/conversations?agent_id=" +
                          urlEncode(agentIt->second);
  const auto resp = client.post(url, "", 15);
  if (resp.code != 200 && resp.code != 201) {
    outErrorMessage = "Failed to create Letta conversation: HTTP " +
                      std::to_string(resp.code);
    if (!resp.body.empty()) {
      outErrorMessage += " " + resp.body;
    }
    return false;
  }

  rapidjson::Document doc;
  doc.Parse(resp.body.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    outErrorMessage = "Invalid Letta conversation creation response";
    return false;
  }

  std::string conversationId;
  if (doc.HasMember("id") && doc["id"].IsString()) {
    conversationId = doc["id"].GetString();
  } else if (doc.HasMember("conversation_id") &&
             doc["conversation_id"].IsString()) {
    conversationId = doc["conversation_id"].GetString();
  }
  if (conversationId.empty()) {
    outErrorMessage = "Missing conversation id in Letta response";
    return false;
  }

  acc.metadata["conversation_id"] = conversationId;
  outConversationId = conversationId;
  if (acc.metadata["ephemeral"] != "1") {
    saveAccounts();
  }
  return true;
}

void LettaProvider::stream(const AgentHistory &history,
                           const ProviderOptions &opts,
                           std::function<void(const StreamEvent &)> onEvent) {
  static constexpr int kMaxRetries = 5;

  for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
    auto accOpt = getAvailableAccount(opts.modelId);
    if (!accOpt) {
      int rateLimitedAccounts = 0;
      int totalAccounts = 0;
      {
        const int64_t now = nowSeconds();
        std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
        totalAccounts = static_cast<int>(accounts_.size());
        for (const auto &existing : accounts_) {
          if (existing.rateLimited && now <= existing.backoffUntil) {
            ++rateLimitedAccounts;
          }
        }
      }

      StreamError se;
      se.message = "No available Letta account";
      if (totalAccounts > 0 && rateLimitedAccounts == totalAccounts) {
        se.message += " (all accounts currently rate-limited/backing off)";
      } else if (rateLimitedAccounts > 0) {
        se.message += " (" + std::to_string(rateLimitedAccounts) +
                      " account(s) currently rate-limited/backing off)";
      }
      onEvent(se);
      return;
    }

    OAuthAccount acc = *accOpt;

    if (isTokenExpired(acc)) {
      if (!refreshAccessToken(acc)) {
        markAccountRateLimited(acc, 60);
        updateAccount(acc);
        continue;
      }
      updateAccount(acc);
    }

    try {
      const int status = executeStreamRequest(acc, history, opts, onEvent);
      updateAccount(acc);
      if (status >= 200 && status < 300) {
        return;
      }

      if (status == 402) {
        acc.metadata["total_balance"] = "0";
        acc.metadata["monthly_credit_balance"] = "0";
        acc.metadata["purchased_credit_balance"] = "0";
        acc.lastQuotaRefresh = nowSeconds();
      }

      const int backoff = (status == 402 || status == 429) ? 60 : (1 << attempt);
      markAccountRateLimited(acc, backoff);
      updateAccount(acc);
      continue;
    } catch (const std::exception &e) {
      int backoff = 1 << attempt; // exponential: 1, 2, 4, 8, 16
      markAccountRateLimited(acc, backoff);
      updateAccount(acc);
      std::this_thread::sleep_for(std::chrono::seconds(backoff));
    }
  }

  StreamError se;
  se.message = "All Letta accounts exhausted after retries";
  onEvent(se);
}

void LettaProvider::generateSummary(
    const std::string &modelId, const AgentHistory &history,
    const std::string &compactionPrompt,
    std::function<void(const StreamEvent &)> onEvent,
    std::atomic<bool> *abortSignal) {

  ProviderOptions opts;
  opts.modelId = modelId.empty() ? "auto-fast" : modelId;
  opts.temperature = 0.3f;
  opts.maxTokens = 4000;
  opts.abortSignal = abortSignal;

  AgentHistory summaryHistory;
  summaryHistory.threadId = history.threadId;

  AgentTurn turn;
  turn.turnId = "summary";

  Message userMsg;
  userMsg.role = firmius::shared::Role::User;
  userMsg.content.push_back(firmius::shared::TextContent{compactionPrompt});
  userMsg.timestamp = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  turn.messages.push_back(userMsg);

  summaryHistory.turns.push_back(turn);

  stream(summaryHistory, opts, onEvent);
}

} // namespace firmius::provider
