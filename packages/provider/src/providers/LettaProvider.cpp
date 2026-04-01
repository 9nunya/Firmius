#include "providers/LettaProvider.hpp"
#include "providers/BackoffConstants.hpp"
#include "utils/GCPHttpClient.hpp"
#include "utils/InterruptibleSleep.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtil.hpp"
#include "utils/TempOAuthServer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iostream>
#include <limits>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <thread>

namespace firmius::provider {

namespace {

constexpr char kProviderId[] = "letta";
constexpr char kClientId[] = "ci-let-724dea7e98f4af6f8f370f4b1466200c";
constexpr char kAuthBaseUrl[] = "https://app.letta.com";
constexpr char kApiBaseUrl[] = "https://api.letta.com";
constexpr char kDeviceCodeEndpoint[] = "/api/oauth/device/code";
constexpr char kTokenEndpoint[] = "/api/oauth/token";
constexpr char kModelsEndpoint[] = "/v1/models";
constexpr char kBalanceEndpoint[] = "/v1/metadata/balance";
constexpr int kQuotaRefreshSeconds = 300;
constexpr float kQuotaAvailableThreshold = 0.01f;

int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string urlEncode(const std::string &value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;
  for (char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      escaped << c;
    } else {
      escaped << '%' << std::setw(2)
              << static_cast<int>(static_cast<unsigned char>(c));
    }
  }
  return escaped.str();
}

} // namespace

// ============================================================================
// OAuthWizard for Letta (Device Code Flow)
// ============================================================================

class LettaOAuthWizard : public OAuthWizard {
public:
  explicit LettaOAuthWizard(LettaProvider *provider) : provider_(provider) {
    // Step 1: Request device code
    GCPHttpClient client("firmius-letta/1.0");
    client.setContentType("application/json");
    std::string body =
        "{\"client_id\":\"" + std::string(kClientId) + "\"}";

    auto resp = client.post(std::string(kAuthBaseUrl) + kDeviceCodeEndpoint,
                            body, 15);
    if (resp.code != 200) {
      error_ = "Failed to request device code: HTTP " +
               std::to_string(resp.code) + " " + resp.body;
      return;
    }

    rapidjson::Document doc;
    doc.Parse(resp.body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
      error_ = "Invalid device code response";
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
      return;
    }

    prompt_ = "Letta OAuth Setup\n\n"
              "1. Open this URL in your browser:\n"
              "   " +
              verificationUriComplete_ + "\n\n"
              "2. Enter this code: " +
              userCode_ + "\n\n"
              "3. Press Enter when you've completed authorization...";
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

  void submitAnswer(const std::string &) override {
    // Start polling for token
    pollForToken();
  }

  bool isComplete() const override { return complete_ || !error_.empty(); }

  bool finalizeExchange(std::string &outErrorMessage) override {
    if (!error_.empty()) {
      outErrorMessage = error_;
      return false;
    }

    if (accessToken_.empty() || refreshToken_.empty()) {
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
    GCPHttpClient client("firmius-letta/1.0");
    client.setContentType("application/json");

    int64_t startTime = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    int pollInterval = interval_;
    int64_t expiresAt = startTime + expiresIn_;

    while (std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count() < expiresAt) {
      std::this_thread::sleep_for(std::chrono::seconds(pollInterval));

      std::string body = "{\"grant_type\":\"urn:ietf:params:oauth:grant-type:"
                         "device_code\",";
      body += "\"client_id\":\"" + std::string(kClientId) + "\",";
      body += "\"device_code\":\"" + deviceCode_ + "\"}";

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
            return;
          }
          if (err == "expired_token") {
            error_ = "Device code expired";
            return;
          }
        }
        error_ = "Token poll failed: HTTP " + std::to_string(resp.code);
        return;
      }

      rapidjson::Document doc;
      doc.Parse(resp.body.c_str());
      if (doc.HasParseError() || !doc.IsObject()) {
        error_ = "Invalid token response";
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
        complete_ = true;
        return;
      }

      error_ = "No access_token in token response";
      return;
    }

    error_ = "OAuth polling timed out";
  }

  static std::string extractEmailFromJwt(const std::string &token) {
    // JWT has 3 parts separated by dots
    size_t firstDot = token.find('.');
    if (firstDot == std::string::npos)
      return "";
    size_t secondDot = token.find('.', firstDot + 1);
    if (secondDot == std::string::npos)
      return "";

    std::string payload = token.substr(firstDot + 1, secondDot - firstDot - 1);
    // Add padding if needed
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

    // Base64 decode (simple implementation)
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

    // Look for email field in JSON
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
  std::string prompt_;
  bool promptShown_ = false;
  bool complete_ = false;
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
        .label = "Auto",
        .description = "Automatically select the best model",
        .contextWindow = 140000,
        .modalities = {"text"},
        .supportsReasoning = true}},
      {"auto-fast",
       {.id = "auto-fast",
        .provider = kProviderId,
        .label = "Auto Fast",
        .description = "Automatically select the best fast model",
        .contextWindow = 140000,
        .modalities = {"text"},
        .supportsReasoning = true}},
      {"auto-chat",
       {.id = "auto-chat",
        .provider = kProviderId,
        .label = "Auto Chat",
        .description = "Automatically select the best model for chat",
        .contextWindow = 140000,
        .modalities = {"text"},
        .supportsReasoning = true}},
      {"sonnet",
       {.id = "sonnet",
        .provider = kProviderId,
        .label = "Sonnet 4.6",
        .description = "Anthropic's Claude Sonnet 4.6 (high reasoning)",
        .contextWindow = 200000,
        .modalities = {"text", "image"},
        .supportsReasoning = true}},
      {"sonnet-1m",
       {.id = "sonnet-1m",
        .provider = kProviderId,
        .label = "Sonnet 4.6 1M",
        .description = "Claude Sonnet 4.6 with 1M token context window",
        .contextWindow = 1000000,
        .modalities = {"text", "image"},
        .supportsReasoning = true}},
      {"opus",
       {.id = "opus",
        .provider = kProviderId,
        .label = "Opus 4.1",
        .description = "Anthropic's most capable model",
        .contextWindow = 200000,
        .modalities = {"text", "image"},
        .supportsReasoning = true}},
      {"gpt-5.2",
       {.id = "gpt-5.2",
        .provider = kProviderId,
        .label = "GPT-5.2",
        .description = "OpenAI GPT-5.2",
        .contextWindow = 272000,
        .modalities = {"text", "image"},
        .supportsReasoning = true}},
      {"gpt-5.1",
       {.id = "gpt-5.1",
        .provider = kProviderId,
        .label = "GPT-5.1",
        .description = "OpenAI GPT-5.1",
        .contextWindow = 128000,
        .modalities = {"text", "image"},
        .supportsReasoning = true}},
      {"gemini-2.5-pro",
       {.id = "gemini-2.5-pro",
        .provider = kProviderId,
        .label = "Gemini 2.5 Pro",
        .description = "Google Gemini 2.5 Pro",
        .contextWindow = 1000000,
        .modalities = {"text", "image"},
        .supportsReasoning = true}},
      {"kimi-k2",
       {.id = "kimi-k2",
        .provider = kProviderId,
        .label = "Kimi K2",
        .description = "Moonshot Kimi K2",
        .contextWindow = 128000,
        .modalities = {"text"},
        .supportsReasoning = true}},
      {"glm-4.6",
       {.id = "glm-4.6",
        .provider = kProviderId,
        .label = "GLM 4.6",
        .description = "Zhipu GLM 4.6",
        .contextWindow = 128000,
        .modalities = {"text"},
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
  // Unknown model - return a generic entry
  return {.id = modelId,
          .provider = kProviderId,
          .contextWindow = 128000,
          .modalities = {"text"},
          .supportsReasoning = true};
}

// ============================================================================
// OAuth
// ============================================================================

std::unique_ptr<OAuthWizard> LettaProvider::beginConnectionWizard() {
  return std::make_unique<LettaOAuthWizard>(this);
}

bool LettaProvider::refreshAccessToken(OAuthAccount &acc) {
  GCPHttpClient client("firmius-letta/1.0");
  client.setContentType("application/json");

  std::string body = "{\"grant_type\":\"refresh_token\",";
  body += "\"client_id\":\"" + std::string(kClientId) + "\",";
  body += "\"refresh_token\":\"" + acc.refreshToken + "\"}";

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

  GCPHttpClient client("firmius-letta/1.0");
  client.setHeader("Authorization", "Bearer " + acc.accessToken);

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
    // Refresh token if expired
    if (isTokenExpired(acc)) {
      if (!refreshAccessToken(acc)) {
        continue;
      }
    }

    // Refresh quotas if stale
    if (acc.lastQuotaRefresh == 0 ||
        (now - acc.lastQuotaRefresh) > kQuotaRefreshSeconds) {
      fetchAndStoreQuotas(acc);
      needsSave = true;
    }

    // Clear stale rate limiting
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
  std::map<std::string, std::vector<QuotaBucket>> result;

  for (const auto &acc : accounts_) {
    std::vector<QuotaBucket> buckets;

    // Balance bucket
    auto balIt = acc.metadata.find("total_balance");
    if (balIt != acc.metadata.end()) {
      QuotaBucket bucket;
      bucket.name = "balance";
      bucket.remainingFraction = 1.0f; // Letta uses credit, not rate limits
      bucket.note = "Balance: $" + balIt->second;

      auto tierIt = acc.metadata.find("billing_tier");
      if (tierIt != acc.metadata.end()) {
        bucket.note += " (Tier: " + tierIt->second + ")";
      }
      buckets.push_back(bucket);
    }

    // Monthly credit bucket
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

// ============================================================================
// SSE Streaming
// ============================================================================

size_t LettaProvider::sseWriteCallback(char *ptr, size_t size, size_t nmemb,
                                       void *userdata) {
  auto *ctx = static_cast<StreamContext *>(userdata);
  size_t total = size * nmemb;

  ctx->buffer.append(ptr, total);

  // Process complete lines
  while (true) {
    size_t newlinePos = ctx->buffer.find('\n', ctx->readOffset);
    if (newlinePos == std::string::npos)
      break;

    std::string line = ctx->buffer.substr(ctx->readOffset,
                                          newlinePos - ctx->readOffset);
    ctx->readOffset = newlinePos + 1;

    // Check for abort
    if (ctx->abortSignal && ctx->abortSignal->load()) {
      return 0; // Signal abort to curl
    }

    ctx->provider->processSSELine(line, *ctx);
  }

  return total;
}

void LettaProvider::processSSELine(const std::string &line,
                                   StreamContext &ctx) {
  // Skip empty lines and comments
  if (line.empty() || line[0] == ':')
    return;

  // Parse SSE data field
  std::string data;
  if (line.rfind("data: ", 0) == 0) {
    data = line.substr(6);
  } else if (line.rfind("data:", 0) == 0) {
    data = line.substr(5);
  } else {
    return; // Not a data line
  }

  if (data == "[DONE]")
    return;

  rapidjson::Document d;
  d.Parse(data.c_str());
  if (d.HasParseError() || !d.IsObject())
    return;

  // Letta uses message_type to distinguish chunk types
  if (!d.HasMember("message_type") || !d["message_type"].IsString())
    return;

  std::string msgType = d["message_type"].GetString();

  // Handle usage_statistics
  if (msgType == "usage_statistics") {
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
    if (d.HasMember("cached_input_tokens") && d["cached_input_tokens"].IsInt()) {
      metrics.tokens.cacheRead = d["cached_input_tokens"].GetInt();
    }
    if (d.HasMember("reasoning_tokens") && d["reasoning_tokens"].IsInt()) {
      metrics.tokens.reasoning = d["reasoning_tokens"].GetInt();
    }
    (*ctx.onEvent)(metrics);
    return;
  }

  // Handle assistant_message (text content)
  if (msgType == "assistant_message") {
    std::string delta;
    if (d.HasMember("content")) {
      if (d["content"].IsString()) {
        delta = d["content"].GetString();
      } else if (d["content"].IsArray()) {
        // Could be array of parts
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
        if (d["content"].HasMember("text") && d["content"]["text"].IsString()) {
          delta = d["content"]["text"].GetString();
        }
      }
    }
    if (!delta.empty()) {
      TextChunk tc;
      tc.text = delta;
      (*ctx.onEvent)(tc);
    }
    return;
  }

  // Handle reasoning_message (thinking/reasoning content)
  if (msgType == "reasoning_message") {
    std::string reason;
    if (d.HasMember("reasoning") && d["reasoning"].IsString()) {
      reason = d["reasoning"].GetString();
    } else if (d.HasMember("content") && d["content"].IsString()) {
      reason = d["content"].GetString();
    }
    if (!reason.empty()) {
      ReasoningChunk rc;
      rc.reasoning = reason;
      (*ctx.onEvent)(rc);
    }
    return;
  }

  // Handle tool_call_message
  if (msgType == "tool_call_message") {
    const auto &toolCall = d["tool_call"];
    if (toolCall.IsObject()) {
      std::string toolCallId, toolName, args;
      if (toolCall.HasMember("tool_call_id") &&
          toolCall["tool_call_id"].IsString())
        toolCallId = toolCall["tool_call_id"].GetString();
      if (toolCall.HasMember("name") && toolCall["name"].IsString())
        toolName = toolCall["name"].GetString();

      if (toolCall.HasMember("arguments")) {
        if (toolCall["arguments"].IsString()) {
          args = toolCall["arguments"].GetString();
        } else {
          rapidjson::StringBuffer sb;
          rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
          toolCall["arguments"].Accept(writer);
          args = sb.GetString();
        }
      }

      // Track tool call state for streaming
      if (!toolCallId.empty()) {
        auto &state = ctx.streamedToolCalls[toolCallId];
        if (!toolName.empty())
          state.lastName = toolName;
        if (!args.empty())
          state.lastArgs += args;

        ToolCallChunk tc;
        tc.id = toolCallId;
        tc.name = toolName;
        tc.args = state.lastArgs;
        (*ctx.onEvent)(tc);
      }
    }
    return;
  }

  // Handle tool_return_message
  if (msgType == "tool_return_message") {
    std::string toolCallId, result, status;
    if (d.HasMember("tool_call_id") && d["tool_call_id"].IsString())
      toolCallId = d["tool_call_id"].GetString();
    if (d.HasMember("status") && d["status"].IsString())
      status = d["status"].GetString();

    // Get result from func_response or tool_return
    if (d.HasMember("func_response")) {
      if (d["func_response"].IsString())
        result = d["func_response"].GetString();
    } else if (d.HasMember("tool_return")) {
      if (d["tool_return"].IsString())
        result = d["tool_return"].GetString();
    }

    if (!toolCallId.empty()) {
      ToolResultChunk tr;
      tr.toolCallId = toolCallId;
      tr.result = result;
      tr.success = (status == "success");
      (*ctx.onEvent)(tr);
    }
    return;
  }

  // Handle api_error
  if (msgType == "api_error" || d.HasMember("error")) {
    std::string errMsg;
    if (d.HasMember("error") && d["error"].IsObject()) {
      const auto &err = d["error"];
      if (err.HasMember("message") && err["message"].IsString())
        errMsg = err["message"].GetString();
    }
    if (!errMsg.empty()) {
      ErrorChunk ec;
      ec.error = errMsg;
      (*ctx.onEvent)(ec);
    }
    return;
  }
}

// ============================================================================
// Stream Execution
// ============================================================================

void LettaProvider::executeStreamRequest(
    OAuthAccount &acc, const AgentHistory &history, const ProviderOptions &opts,
    std::function<void(const StreamEvent &)> &onEvent) {

  CURL *curl = curl_easy_init();
  if (!curl)
    return;

  // Build the request URL - Letta uses agent-based API
  // We need to send a message to the agent conversation
  std::string url = std::string(kApiBaseUrl) + "/v1/agents";

  // Build JSON request body
  rapidjson::Document doc(rapidjson::kObjectType);
  rapidjson::Document::AllocatorType &alloc = doc.GetAllocator();

  // Build messages array from history
  rapidjson::Value messages(rapidjson::kArrayType);

  // Add system prompt from history
  if (!history.turns.empty() && !history.turns[0].messages.empty()) {
    rapidjson::Value sysMsg(rapidjson::kObjectType);
    sysMsg.AddMember("role", "system", alloc);
    std::string sysContent;
    for (const auto &msg : history.turns[0].messages) {
      if (msg.role == "system") {
        if (!sysContent.empty())
          sysContent += "\n";
        sysContent += msg.content;
      }
    }
    if (sysContent.empty())
      sysContent = "You are a helpful coding assistant.";
    sysMsg.AddMember("content",
                     rapidjson::Value(sysContent.c_str(), alloc), alloc);
    messages.PushBack(sysMsg, alloc);
  }

  // Add conversation messages
  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == "system")
        continue;

      rapidjson::Value chatMsg(rapidjson::kObjectType);
      std::string role = (msg.role == "user") ? "user" : "assistant";
      chatMsg.AddMember("role",
                        rapidjson::Value(role.c_str(), alloc), alloc);
      chatMsg.AddMember("content",
                        rapidjson::Value(msg.content.c_str(), alloc), alloc);

      // Add tool_calls if present
      if (!msg.toolCalls.empty()) {
        rapidjson::Value toolCallsArr(rapidjson::kArrayType);
        for (const auto &tc : msg.toolCalls) {
          rapidjson::Value tcObj(rapidjson::kObjectType);
          tcObj.AddMember("id",
                          rapidjson::Value(tc.id.c_str(), alloc), alloc);
          tcObj.AddMember("type", "function", alloc);
          rapidjson::Value func(rapidjson::kObjectType);
          func.AddMember("name",
                         rapidjson::Value(tc.name.c_str(), alloc), alloc);
          func.AddMember("arguments",
                         rapidjson::Value(tc.args.c_str(), alloc), alloc);
          tcObj.AddMember("function", func, alloc);
          toolCallsArr.PushBack(tcObj, alloc);
        }
        chatMsg.AddMember("tool_calls", toolCallsArr, alloc);
      }

      // Add tool_call_id if present
      if (!msg.toolCallId.empty()) {
        chatMsg.AddMember("tool_call_id",
                          rapidjson::Value(msg.toolCallId.c_str(), alloc),
                          alloc);
      }

      messages.PushBack(chatMsg, alloc);
    }
  }

  doc.AddMember("messages", messages, alloc);

  // Model
  std::string modelId = opts.modelId.empty() ? "auto" : opts.modelId;
  doc.AddMember("model",
                rapidjson::Value(modelId.c_str(), alloc), alloc);

  // Stream
  doc.AddMember("stream", true, alloc);

  // Temperature
  doc.AddMember("temperature", opts.temperature, alloc);

  // Max tokens
  if (opts.maxTokens.has_value()) {
    doc.AddMember("max_tokens",
                  static_cast<int>(opts.maxTokens.value()), alloc);
  }

  // Tools
  if (!opts.tools.empty()) {
    rapidjson::Value toolsArr(rapidjson::kArrayType);
    for (const auto &tool : opts.tools) {
      rapidjson::Value toolObj(rapidjson::kObjectType);
      toolObj.AddMember("type", "function", alloc);

      rapidjson::Value func(rapidjson::kObjectType);
      func.AddMember("name",
                     rapidjson::Value(tool.name.c_str(), alloc), alloc);
      func.AddMember("description",
                     rapidjson::Value(tool.description.c_str(), alloc),
                     alloc);

      rapidjson::Document schemaDoc;
      schemaDoc.Parse(tool.inputSchema.c_str());
      if (!schemaDoc.HasParseError()) {
        rapidjson::Value schemaVal;
        schemaVal.CopyFrom(schemaDoc, alloc);
        func.AddMember("parameters", schemaVal, alloc);
      }

      toolObj.AddMember("function", func, alloc);
      toolsArr.PushBack(toolObj, alloc);
    }
    doc.AddMember("tools", toolsArr, alloc);
  }

  // Serialize
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  std::string body = buffer.GetString();

  // Setup curl
  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(
      headers, ("Authorization: Bearer " + acc.accessToken).c_str());
  headers = curl_slist_append(headers, "Accept: text/event-stream");

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

  CURLcode res = curl_easy_perform(curl);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK && res != CURLE_ABORTED_BY_CALLBACK) {
    ErrorChunk ec;
    ec.error = std::string("curl error: ") + curl_easy_strerror(res);
    onEvent(ec);
  }
}

void LettaProvider::stream(const AgentHistory &history,
                           const ProviderOptions &opts,
                           std::function<void(const StreamEvent &)> onEvent) {
  // Retry logic with account rotation
  static constexpr int kMaxRetries = 5;

  for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
    auto accOpt = getAvailableAccount(opts.modelId);
    if (!accOpt) {
      ErrorChunk ec;
      ec.error = "No available Letta account";
      onEvent(ec);
      return;
    }

    OAuthAccount *acc = *accOpt;

    // Refresh token if needed
    if (isTokenExpired(*acc)) {
      if (!refreshAccessToken(*acc)) {
        markAccountRateLimited(*acc, 60);
        continue;
      }
    }

    try {
      executeStreamRequest(*acc, history, opts, onEvent);
      return; // Success
    } catch (const std::exception &e) {
      int backoff = BackoffConstants::computeBackoff(attempt);
      markAccountRateLimited(*acc, backoff);
      InterruptibleSleep::sleepFor(backoff);
    }
  }

  ErrorChunk ec;
  ec.error = "All Letta accounts exhausted after retries";
  onEvent(ec);
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

  // Build a simple history with just the compaction prompt
  AgentHistory summaryHistory;
  summaryHistory.threadId = history.threadId;

  AgentTurn turn;
  turn.turnId = "summary";

  Message userMsg;
  userMsg.role = "user";
  userMsg.content = compactionPrompt;
  turn.messages.push_back(userMsg);

  summaryHistory.turns.push_back(turn);

  stream(summaryHistory, opts, onEvent);
}

} // namespace firmius::provider
