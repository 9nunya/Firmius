#include "providers/AntigravityProvider.hpp"
#include "providers/AntigravityProtocol.hpp"
#include "utils/GCPHttpClient.hpp"
#include "utils/TempOAuthServer.hpp"
#include <atomic>
#include <chrono>
#include <cmath>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <thread>

namespace firmius::provider {

using namespace firmius::utils;

namespace {

float normalizeQuotaFraction(double value) {
  if (!std::isfinite(value))
    return 0.0f;
  double normalized = value;
  if (normalized > 1.0)
    normalized = normalized / 100.0;
  if (normalized < 0.0)
    normalized = 0.0;
  if (normalized > 1.0)
    normalized = 1.0;
  return static_cast<float>(normalized);
}

struct StreamContext {
  AntigravityProvider *provider;
  std::function<void(const StreamEvent &)> *onEvent;
  std::string buffer;
  size_t readOffset = 0;
  std::atomic<bool> *abortSignal;
};

} // namespace

size_t AntigravityProvider::sseWriteCallback(char *ptr, size_t size,
                                             size_t nmemb, void *userdata) {
  auto *ctx = static_cast<StreamContext *>(userdata);
  if (ctx->abortSignal && ctx->abortSignal->load())
    return 0;
  ctx->buffer.append(ptr, size * nmemb);

  size_t newlinePos;
  while ((newlinePos = ctx->buffer.find('\n', ctx->readOffset)) !=
         std::string::npos) {
    std::string_view line(ctx->buffer.data() + ctx->readOffset,
                          newlinePos - ctx->readOffset);
    ctx->readOffset = newlinePos + 1;

    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    if (line.empty())
      continue;

    ctx->provider->processSSELine(std::string(line), *(ctx->onEvent));
  }

  if (ctx->readOffset > 1024 * 1024) {
    ctx->buffer.erase(0, ctx->readOffset);
    ctx->readOffset = 0;
  }
  return size * nmemb;
}

class AntigravityOAuthWizard : public OAuthWizard {
public:
  AntigravityOAuthWizard(AntigravityProvider *provider) : provider_(provider) {
    std::string url =
        "https://accounts.google.com/o/oauth2/v2/auth?"
        "client_id=1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps."
        "googleusercontent.com"
        "&response_type=code"
        "&redirect_uri=http://localhost:51121/oauth-callback"
        "&scope="
        "https://www.googleapis.com/auth/cloud-platform%20"
        "https://www.googleapis.com/auth/userinfo.email%20"
        "https://www.googleapis.com/auth/userinfo.profile%20"
        "https://www.googleapis.com/auth/cclog%20"
        "https://www.googleapis.com/auth/experimentsandconfigs"
        "&access_type=offline"
        "&prompt=consent";
    prompt_ = "Please open this URL in your browser:\n\n" + url +
              "\n\nWaiting for authorization response...";

    server_.startAsync(
        "<html><body><h1>Firmius Temp Server</h1><p>Authentication complete! "
        "You may now close this window and return to "
        "Firmius.</p></body></html>");
  }

  std::optional<WizardPrompt> nextPrompt() override {
    if (!promptShown_) {
      promptShown_ = true;
      return WizardPrompt{prompt_, false};
    }
    return std::nullopt;
  }

  void submitAnswer(const std::string &) override {}

  bool isComplete() const override { return server_.hasReceivedCode(); }

  bool finalizeExchange(std::string &outErrorMessage) override {
    authCode_ = server_.getCode();
    server_.stop();
    if (authCode_.empty()) {
      outErrorMessage = "OAuth authorization failed: No code received.";
      return false;
    }

    GCPHttpClient client;
    client.setContentType("application/x-www-form-urlencoded");
    std::string body =
        "grant_type=authorization_code"
        "&client_id=1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps."
        "googleusercontent.com"
        "&client_secret=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"
        "&redirect_uri=http://localhost:51121/oauth-callback"
        "&code=" +
        authCode_;

    auto resp = client.post("https://oauth2.googleapis.com/token", body, 10);
    if (resp.code != 200) {
      outErrorMessage = "Token exchange failed: HTTP " +
                        std::to_string(resp.code) + " " + resp.body;
      return false;
    }

    rapidjson::Document doc;
    doc.Parse(resp.body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
      outErrorMessage = "Invalid JSON response from token endpoint";
      return false;
    }

    OAuthAccount acc;
    if (doc.HasMember("access_token") && doc["access_token"].IsString()) {
      acc.accessToken = doc["access_token"].GetString();
    }
    if (doc.HasMember("expires_in") && doc["expires_in"].IsInt()) {
      acc.tokenExpiration =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count() +
          doc["expires_in"].GetInt();
    }
    if (doc.HasMember("refresh_token") && doc["refresh_token"].IsString()) {
      acc.refreshToken = doc["refresh_token"].GetString();
    }

    if (acc.accessToken.empty() || acc.refreshToken.empty()) {
      outErrorMessage = "Missing access or refresh token in payload";
      return false;
    }

    // Fetch user email
    client.clearHeaders();
    client.setBearerToken(acc.accessToken);
    auto uResp =
        client.get("https://www.googleapis.com/oauth2/v3/userinfo", 10);
    if (uResp.code == 200) {
      rapidjson::Document uDoc;
      uDoc.Parse(uResp.body.c_str());
      if (!uDoc.HasParseError() && uDoc.IsObject() && uDoc.HasMember("email") &&
          uDoc["email"].IsString()) {
        acc.identifier = uDoc["email"].GetString();
      }
    }

    provider_->addAccount(acc);
    return true;
  }

  std::string getFinalMessage() const override {
    return "Successfully authenticated with Antigravity!";
  }

private:
  AntigravityProvider *provider_;
  std::string prompt_;
  bool promptShown_ = false;
  mutable std::string authCode_;
  TempOAuthServer server_;
};

AntigravityProvider::AntigravityProvider() : BaseOAuthProvider("antigravity") {}

std::map<std::string, ModelInfo> AntigravityProvider::getStaticModels() {
  std::vector<ModelVariant> variants = {{"low", "{\"effort\":\"low\"}"},
                                        {"medium", "{\"effort\":\"medium\"}"},
                                        {"high", "{\"effort\":\"high\"}"},
                                        {"max", "{\"effort\":\"max\"}"}};

  return {{"gemini-3-flash",
           {.id = "gemini-3-flash",
            .provider = "antigravity",
            .contextWindow = 1000000,
            .modalities = {"text", "image"},
            .variants = variants,
            .supportsReasoning = true}},
          {"gemini-3.1-pro",
           {.id = "gemini-3.1-pro",
            .provider = "antigravity",
            .contextWindow = 1000000,
            .modalities = {"text", "image"},
            .variants = variants,
            .supportsReasoning = true}},
          {"claude-sonnet-4-6",
           {.id = "claude-sonnet-4-6",
            .provider = "antigravity",
            .contextWindow = 200000,
            .modalities = {"text", "image"},
            .variants = variants,
            .supportsReasoning = true}},
          {"claude-opus-4-6-thinking",
           {.id = "claude-opus-4-6-thinking",
            .provider = "antigravity",
            .contextWindow = 200000,
            .modalities = {"text", "image"},
            .variants = variants,
            .supportsReasoning = true}}};
}

std::vector<ModelInfo> AntigravityProvider::listModels() {
  std::vector<ModelInfo> result;
  for (const auto &[id, info] : getStaticModels())
    result.push_back(info);
  return result;
}

ModelInfo AntigravityProvider::getModelInfo(const std::string &modelId) {
  auto models = getStaticModels();
  if (models.count(modelId))
    return models[modelId];
  return {.id = modelId,
          .provider = "antigravity",
          .contextWindow = 8192,
          .modalities = {"text"},
          .variants = {}};
}

std::unique_ptr<OAuthWizard> AntigravityProvider::beginConnectionWizard() {
  return std::make_unique<AntigravityOAuthWizard>(this);
}

std::optional<OAuthAccount *> AntigravityProvider::getAvailableAccount(
    const std::optional<std::string> &modelId) {
  if (accounts_.empty())
    return std::nullopt;

  std::string group = "unknown";
  if (modelId) {
    std::string lowerModel = *modelId;
    for (auto &c : lowerModel)
      c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    if (lowerModel.find("claude") != std::string::npos)
      group = "claude";
    else if (lowerModel.find("gemini-3") != std::string::npos ||
             lowerModel.find("gemini 3") != std::string::npos ||
             lowerModel.find("gemini-2.5") != std::string::npos) {
      if (lowerModel.find("flash") != std::string::npos)
        group = "gemini-flash";
      else
        group = "gemini-pro";
    }
  }

  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  int startIdx = (lastUsedIndex_ >= 0) ? lastUsedIndex_ : 0;
  if (startIdx >= static_cast<int>(accounts_.size())) {
    startIdx = 0;
  }
  int currentIdx = startIdx;

  if (group != "unknown") {
    do {
      auto &acc = accounts_[currentIdx];
      if (acc.rateLimited && now > acc.backoffUntil) {
        acc.rateLimited = false;
        // Reset stale quota metadata so the account is not skipped again
        for (auto it = acc.metadata.begin(); it != acc.metadata.end(); ++it) {
          if (it->first.rfind("quota:", 0) == 0 && it->second == "0") {
            it->second = "1";
          }
        }
      }
      if (!acc.rateLimited) {
        bool hasQuota = false;
        for (const auto &[key, val] : acc.metadata) {
          if (key.rfind("quota:", 0) == 0) {
            std::string modelGroup = "unknown";
            std::string lowerModel = key.substr(6);
            for (auto &c : lowerModel)
              c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

            if (lowerModel.find("claude") != std::string::npos)
              modelGroup = "claude";
            else if (lowerModel.find("gemini-3") != std::string::npos ||
                     lowerModel.find("gemini 3") != std::string::npos ||
                     lowerModel.find("gemini-2.5") != std::string::npos) {
              if (lowerModel.find("flash") != std::string::npos)
                modelGroup = "gemini-flash";
              else
                modelGroup = "gemini-pro";
            }

            if (modelGroup == group) {
              try {
                if (std::stof(val) > 0.01f) {
                  hasQuota = true;
                  break;
                }
              } catch (...) {
              }
            }
          }
        }

        if (hasQuota) {
          if (!isTokenExpired(acc) || refreshAccessToken(acc)) {
            lastUsedIndex_ = currentIdx;
            saveAccounts();
            return &acc;
          }
        }
      }
      currentIdx = (currentIdx + 1) % static_cast<int>(accounts_.size());
    } while (currentIdx != startIdx);
  }

  return BaseOAuthProvider::getAvailableAccount(modelId);
}

bool AntigravityProvider::refreshAccessToken(OAuthAccount &acc) {
  GCPHttpClient client;
  client.setContentType("application/x-www-form-urlencoded");
  std::string body =
      "grant_type=refresh_token"
      "&client_id=1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps."
      "googleusercontent.com"
      "&client_secret=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"
      "&refresh_token=" +
      acc.refreshToken;

  auto resp = client.post("https://oauth2.googleapis.com/token", body, 10);
  if (resp.code == 200) {
    rapidjson::Document doc;
    doc.Parse(resp.body.c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      bool updated = false;
      if (doc.HasMember("access_token") && doc["access_token"].IsString()) {
        acc.accessToken = doc["access_token"].GetString();
        updated = true;
      }
      if (doc.HasMember("expires_in") && doc["expires_in"].IsInt()) {
        acc.tokenExpiration =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count() +
            doc["expires_in"].GetInt();
      }
      if (doc.HasMember("refresh_token") && doc["refresh_token"].IsString()) {
        acc.refreshToken = doc["refresh_token"].GetString();
      }
      if (updated) {
        saveAccounts();
        return true;
      }
    }
  }
  return false;
}

void AntigravityProvider::processSSELine(
    const std::string &line,
    std::function<void(const StreamEvent &)> &onEvent) {
  if (line.empty() || line[0] == ':')
    return;
  std::string data = line.starts_with("data: ") ? line.substr(6) : line;
  if (data == "[DONE]")
    return;

  rapidjson::Document d;
  d.Parse(data.c_str());
  if (d.HasParseError() || !d.IsObject() || !d.HasMember("response"))
    return;

  const auto &resp = d["response"];
  if (resp.HasMember("usageMetadata")) {
    const auto &usage = resp["usageMetadata"];
    AgentMetrics metrics;
    if (usage.HasMember("promptTokenCount")) {
      metrics.tokens.prompt = usage["promptTokenCount"].GetUint();
      metrics.tokens.contextSize = metrics.tokens.prompt;
    }
    if (usage.HasMember("candidatesTokenCount"))
      metrics.tokens.completion = usage["candidatesTokenCount"].GetUint();
    if (usage.HasMember("totalTokenCount"))
      metrics.tokens.total = usage["totalTokenCount"].GetUint();
    if (usage.HasMember("cachedContentTokenCount"))
      metrics.tokens.cacheRead = usage["cachedContentTokenCount"].GetUint();
    if (usage.HasMember("thoughtsTokenCount"))
      metrics.tokens.reasoning = usage["thoughtsTokenCount"].GetUint();
    onEvent(metrics);
  }

  if (resp.HasMember("candidates") && resp["candidates"].IsArray() &&
      resp["candidates"].Size() > 0) {
    const auto &cand = resp["candidates"][0];
    if (cand.HasMember("content") && cand["content"].HasMember("parts")) {
      for (const auto &part : cand["content"]["parts"].GetArray()) {
        if (part.HasMember("text")) {
          std::string text = part["text"].GetString();
          bool isThinking = false;
          if (part.HasMember("thought"))
            isThinking = part["thought"].GetBool();
          else if (part.HasMember("type"))
            isThinking = (std::string(part["type"].GetString()) == "thinking");

          if (isThinking)
            onEvent(ThinkingChunk{text});
          else
            onEvent(TextChunk{text});
        } else if (part.HasMember("functionCall")) {
          const auto &fc = part["functionCall"];
          ToolCallChunk chunk;
          if (fc.HasMember("name"))
            chunk.nameDelta = fc["name"].GetString();
          if (fc.HasMember("args")) {
            rapidjson::StringBuffer sb;
            rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
            fc["args"].Accept(writer);
            chunk.argsDelta = sb.GetString();
          }
          onEvent(chunk);
        }
      }
    }
  }
}

void AntigravityProvider::stream(
    const AgentHistory &history, const ProviderOptions &opts,
    std::function<void(const StreamEvent &)> onEvent) {
  int accountRetries = 0;
  std::string lastError;
  std::string lastAccountEmail;
  int maxRetries = std::max(5, static_cast<int>(accounts_.size()) * 3);
  while (accountRetries < maxRetries) {
    auto optAcc = getAvailableAccount(opts.modelId);
    if (!optAcc) {
      // All accounts rate-limited: find the one closest to unlocking
      int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
      int64_t earliestUnlock = 0;
      for (const auto &a : accounts_) {
        if (a.rateLimited) {
          if (earliestUnlock == 0 || a.backoffUntil < earliestUnlock)
            earliestUnlock = a.backoffUntil;
        }
      }
      int64_t waitSec = (earliestUnlock > now) ? (earliestUnlock - now) : 0;
      if (waitSec > 120)
        waitSec = 120; // cap at 2 minutes
      if (waitSec > 0) {
        onEvent(StreamRetrying{accountRetries + 1, maxRetries, 429,
                               static_cast<int>(waitSec * 1000),
                               "All accounts rate-limited, waiting",
                               lastAccountEmail});
        std::this_thread::sleep_for(std::chrono::seconds(waitSec));
        // Clear expired backoffs after sleeping
        int64_t nowAfter =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        for (auto &a : accounts_) {
          if (a.rateLimited && nowAfter > a.backoffUntil) {
            a.rateLimited = false;
            for (auto &[k, v] : a.metadata) {
              if (k.rfind("quota:", 0) == 0 && v == "0")
                v = "1";
            }
          }
        }
        accountRetries++;
        continue;
      }
      // Nothing to wait for
      if (!lastError.empty()) {
        onEvent(StreamError{lastError, -1, lastAccountEmail});
      } else {
        onEvent(StreamError{"No accounts available.", -1, lastAccountEmail});
      }
      return;
    }
    OAuthAccount &acc = *optAcc.value();
    lastAccountEmail = acc.getIdentifier();

    if (accountRetries > 0) {
      onEvent(StreamAccountSwitched{acc.getIdentifier()});
    }

    for (int retryAttempt = 0; retryAttempt < 4; ++retryAttempt) {
      if (retryAttempt > 0) {
        onEvent(StreamRetrying{retryAttempt, 4, 0,
                               (1 << (retryAttempt - 1)) * 1000,
                               "Connection error", acc.getIdentifier()});
        std::this_thread::sleep_for(
            std::chrono::seconds(1 << (retryAttempt - 1)));
      }

      std::string effectiveModel =
          opts.modelId.empty() ? "gemini-3-flash" : opts.modelId;

      std::string effectiveProjectId = acc.metadata.count("projectId")
                                           ? acc.metadata["projectId"]
                                           : "rising-fact-p41fc";

      AntigravityProtocol::RequestContext reqCtx;
      reqCtx.modelId = effectiveModel;
      reqCtx.projectId = effectiveProjectId;
      reqCtx.sessionId = std::to_string(rand());
      reqCtx.requestId = "agent-" + std::to_string(rand());

      std::string body =
          AntigravityProtocol::prepareRequestBody(history, opts, reqCtx);

      GCPHttpClient client;
      client.setBearerToken(acc.accessToken);

      std::uint64_t startMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      std::uint64_t firstTokenMs = 0;
      bool firstTokenEmitted = false;
      bool metricsReceived = false;
      AgentMetrics capturedMetrics;

      auto wrappedOnEvent = [&](const StreamEvent &ev) {
        if (std::holds_alternative<TextChunk>(ev)) {
          if (!firstTokenEmitted) {
            firstTokenMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            firstTokenEmitted = true;
          }
          onEvent(ev);
        } else if (std::holds_alternative<ThinkingChunk>(ev)) {
          if (!firstTokenEmitted) {
            firstTokenMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            firstTokenEmitted = true;
          }
          onEvent(ev);
        } else if (auto *met = std::get_if<AgentMetrics>(&ev)) {
          capturedMetrics = *met;
          metricsReceived = true;
        } else {
          onEvent(ev);
        }
      };

      std::function<void(const StreamEvent &)> wrappedFn = wrappedOnEvent;
      StreamContext ctx{this, &wrappedFn, "", 0, opts.abortSignal};
      auto resp =
          client.streamPost("https://daily-cloudcode-pa.sandbox.googleapis.com/"
                            "v1internal:streamGenerateContent?alt=sse",
                            body, sseWriteCallback, &ctx);

      if (resp.code == 200) {
        auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
        if (metricsReceived) {
          capturedMetrics.timing.startMs = startMs;
          capturedMetrics.timing.firstTokenMs =
              firstTokenEmitted ? firstTokenMs : 0;
          capturedMetrics.timing.endMs = endMs;

          try {
            auto modelInfo = getModelInfo(opts.modelId);
            double promptUsd =
                (static_cast<double>(capturedMetrics.tokens.prompt) *
                 modelInfo.pricePer1MInput) /
                1000000.0;
            double completionUsd =
                (static_cast<double>(capturedMetrics.tokens.completion) *
                 modelInfo.pricePer1MOutput) /
                1000000.0;
            double cacheReadUsd =
                (static_cast<double>(capturedMetrics.tokens.cacheRead) *
                 modelInfo.pricePer1MCacheRead) /
                1000000.0;
            capturedMetrics.estimatedCostUsd =
                promptUsd + completionUsd + cacheReadUsd;
          } catch (...) {
          }

          onEvent(capturedMetrics);
        }
        return;
      }
      if (resp.code == 402 || resp.code == 429 || resp.code == 403) {
        if (resp.code == 403) {
          lastError = "API error 403: " + ctx.buffer;
        } else {
          lastError = "Rate limited (HTTP " + std::to_string(resp.code) + ")";
        }
        int backoff = std::min(60, 1 << accountRetries);
        markAccountRateLimited(acc, backoff);
        break;
      }
      if (resp.code < 500 && resp.code != 408 && resp.code != 0) {
        std::string errMsg = "API error: " + std::to_string(resp.code);
        if (!ctx.buffer.empty())
          errMsg += "\n" + ctx.buffer;
        onEvent(StreamError{errMsg, (int)resp.code, acc.getIdentifier()});
        return;
      }
    }
    accountRetries++;
  }

  if (!lastError.empty()) {
    onEvent(StreamError{lastError, -1, lastAccountEmail});
  } else {
    onEvent(StreamError{"Exhausted retries.", -1, lastAccountEmail});
  }
}

void AntigravityProvider::generateSummary(
    const std::string &modelId, const AgentHistory &history,
    const std::string &, std::function<void(const StreamEvent &)> onEvent,
    std::atomic<bool> *abortSignal) {
  firmius::provider::ProviderOptions opts;
  opts.modelId = modelId;
  opts.temperature = 0.7f;
  opts.maxTokens = 16384;
  opts.abortSignal = abortSignal;
  stream(history, opts, onEvent);
}

void AntigravityProvider::refreshQuotas() {
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  for (auto &acc : accounts_) {
    if (now - acc.lastQuotaRefresh >= 7200) {
      if (isTokenExpired(acc))
        refreshAccessToken(acc);
      fetchAndStoreQuotas(acc);
    }
  }
}

std::string AntigravityProvider::fetchManagedProject(OAuthAccount &acc) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &a = doc.GetAllocator();
  rapidjson::Value metadata(rapidjson::kObjectType);
  metadata.AddMember("ideType", "ANTIGRAVITY", a);
  metadata.AddMember("platform", "LINUX", a);
  metadata.AddMember("pluginType", "GEMINI", a);
  rapidjson::Value bodyObj(rapidjson::kObjectType);
  bodyObj.AddMember("metadata", metadata, a);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  bodyObj.Accept(writer);

  GCPHttpClient client;
  client.setBearerToken(acc.accessToken);
  client.addHeader("User-Agent", "google-api-nodejs-client/9.15.1");
  client.addHeader("X-Goog-Api-Client",
                   "google-cloud-sdk vscode_cloudshelleditor/0.1");

  auto resp = client.post(
      "https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist",
      buffer.GetString());
  if (resp.code == 200) {
    rapidjson::Document respDoc;
    respDoc.Parse(resp.body.c_str());
    if (!respDoc.HasParseError() && respDoc.IsObject() &&
        respDoc.HasMember("cloudaicompanionProject")) {
      const auto &proj = respDoc["cloudaicompanionProject"];
      if (proj.IsString())
        return proj.GetString();
      if (proj.IsObject() && proj.HasMember("id"))
        return proj["id"].GetString();
    }
  }
  return "";
}

void AntigravityProvider::fetchAndStoreQuotas(OAuthAccount &acc) {
  std::string projId =
      acc.metadata.count("managedProjectId")
          ? acc.metadata["managedProjectId"]
          : (acc.metadata.count("projectId") ? acc.metadata["projectId"] : "");
  if (projId.empty()) {
    projId = fetchManagedProject(acc);
    if (!projId.empty()) {
      acc.metadata["managedProjectId"] = projId;
      saveAccounts();
    }
  }
  if (projId.empty())
    projId = "rising-fact-p41fc";

  GCPHttpClient client("antigravity/1.18.3 windows/amd64");
  client.setBearerToken(acc.accessToken);

  auto resp = client.post(
      "https://cloudcode-pa.googleapis.com/v1internal:fetchAvailableModels",
      "{\"project\":\"" + projId + "\"}", 10);
  if (resp.code == 200) {
    rapidjson::Document doc;
    doc.Parse(resp.body.c_str());
    if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("models")) {
      for (auto it = doc["models"].MemberBegin();
           it != doc["models"].MemberEnd(); ++it) {
        if (it->value.HasMember("quotaInfo")) {
          const auto &q = it->value["quotaInfo"];
          float remaining =
              normalizeQuotaFraction(q["remainingFraction"].GetDouble());
          acc.metadata["quota:" + std::string(it->name.GetString())] =
              std::to_string(remaining);
          if (q.HasMember("resetTime"))
            acc.metadata["quota_reset:" + std::string(it->name.GetString())] =
                q["resetTime"].GetString();
        }
      }
      acc.lastQuotaRefresh =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      saveAccounts();
    }
  }
}

std::map<std::string, std::vector<QuotaBucket>>
AntigravityProvider::getAllQuotas() const {
  std::map<std::string, std::vector<QuotaBucket>> result;
  for (const auto &acc : accounts_) {
    std::vector<QuotaBucket> buckets;
    std::map<std::string, std::pair<float, std::string>> groups;
    for (const auto &[key, val] : acc.metadata) {
      if (key.rfind("quota:", 0) == 0) {
        std::string model = key.substr(6);
        std::string g = "unknown";
        if (model.find("claude") != std::string::npos)
          g = "claude";
        else if (model.find("gemini") != std::string::npos)
          g = (model.find("flash") != std::string::npos) ? "gemini-flash"
                                                         : "gemini-pro";

        if (g != "unknown") {
          float f = std::stof(val);
          if (groups.find(g) == groups.end() || f < groups[g].first) {
            std::string reset = acc.metadata.count("quota_reset:" + model)
                                    ? acc.metadata.at("quota_reset:" + model)
                                    : "";
            groups[g] = {f, reset};
          }
        }
      }
    }
    for (auto const &[name, data] : groups)
      buckets.push_back({name, data.first, data.second});
    result[acc.getIdentifier()] = buckets;
  }
  return result;
}

} // namespace firmius::provider
