#include "providers/antigravity/AntigravityProvider.hpp"
#include "utils/TempOAuthServer.hpp"
#include <chrono>
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <thread>

namespace firmius::provider {

namespace {

// Same streaming chunk extractor as BaseOpenAIProvider
struct StreamContext {
  AntigravityProvider *provider;
  std::function<void(const StreamEvent &)> *onEvent;
  std::string buffer;
  size_t readOffset = 0;
  std::atomic<bool> *abortSignal;
};

size_t sseWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
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

} // namespace

std::string roleToString(Role r) {
  switch (r) {
  case Role::System:
    return "system";
  case Role::User:
    return "user";
  case Role::Assistant:
    return "model";
  case Role::ToolResult:
    return "user"; // Antigravity/Gemini CLI uses 'user' role for function
                   // response parts
  }
  return "user";
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

    // Start temporary localhost server to automatically capture the redirect
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

  void submitAnswer(const std::string & /*answer*/) override {
    // Only used if fallback manual mode is triggered, otherwise ignored
  }

  bool isComplete() const override {
    if (server_.hasReceivedCode()) {
      return true;
    }
    return false;
  }

  bool finalizeExchange(std::string &outErrorMessage) override {
    authCode_ = server_.getCode();
    server_.stop();
    if (authCode_.empty()) {
      outErrorMessage = "OAuth authorization failed: No code received.";
      return false;
    }
    std::string url = "https://oauth2.googleapis.com/token";
    std::string body =
        "grant_type=authorization_code"
        "&client_id=1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps."
        "googleusercontent.com"
        "&client_secret=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"
        "&redirect_uri=http://localhost:51121/oauth-callback"
        "&code=" +
        authCode_;

    CURL *curl = curl_easy_init();
    if (!curl) {
      outErrorMessage = "Failed to initialize CURL";
      return false;
    }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(
        headers, "Content-Type: application/x-www-form-urlencoded");

    std::string responseBuffer;
    auto writer = [](char *ptr, size_t size, size_t nmemb,
                     void *userdata) -> size_t {
      static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
      return size * nmemb;
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION,
        static_cast<size_t (*)(char *, size_t, size_t, void *)>(writer));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long responseCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || responseCode != 200) {
      outErrorMessage = "Token exchange failed: HTTP " +
                        std::to_string(responseCode) + " " + responseBuffer;
      return false;
    }

    rapidjson::Document doc;
    doc.Parse(responseBuffer.c_str());
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
    std::string userinfoUrl = "https://www.googleapis.com/oauth2/v3/userinfo";
    CURL *uCurl = curl_easy_init();
    if (uCurl) {
      struct curl_slist *uHeaders = nullptr;
      uHeaders = curl_slist_append(
          uHeaders, ("Authorization: Bearer " + acc.accessToken).c_str());

      std::string uResponseBuffer;
      curl_easy_setopt(uCurl, CURLOPT_URL, userinfoUrl.c_str());
      curl_easy_setopt(uCurl, CURLOPT_HTTPHEADER, uHeaders);
      curl_easy_setopt(
          uCurl, CURLOPT_WRITEFUNCTION,
          static_cast<size_t (*)(char *, size_t, size_t, void *)>(writer));
      curl_easy_setopt(uCurl, CURLOPT_WRITEDATA, &uResponseBuffer);
      curl_easy_setopt(uCurl, CURLOPT_TIMEOUT, 10L);

      CURLcode uRes = curl_easy_perform(uCurl);
      long uResponseCode = 0;
      curl_easy_getinfo(uCurl, CURLINFO_RESPONSE_CODE, &uResponseCode);

      if (uRes == CURLE_OK && uResponseCode == 200) {
        rapidjson::Document uDoc;
        uDoc.Parse(uResponseBuffer.c_str());
        if (!uDoc.HasParseError() && uDoc.IsObject() &&
            uDoc.HasMember("email") && uDoc["email"].IsString()) {
          acc.email = uDoc["email"].GetString();
        }
      }
      curl_slist_free_all(uHeaders);
      curl_easy_cleanup(uCurl);
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

rapidjson::Value
AntigravityProvider::toGeminiSchema(const std::string &inputSchema,
                                    rapidjson::Document::AllocatorType &a) {
  rapidjson::Document doc;
  doc.Parse(inputSchema.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    rapidjson::Value fallback(rapidjson::kObjectType);
    fallback.AddMember("type", "OBJECT", a);
    return fallback;
  }

  std::function<rapidjson::Value(const rapidjson::Value &)> transform;
  transform = [&](const rapidjson::Value &val) -> rapidjson::Value {
    if (val.IsObject()) {
      rapidjson::Value out(rapidjson::kObjectType);
      for (auto it = val.MemberBegin(); it != val.MemberEnd(); ++it) {
        std::string key = it->name.GetString();
        if (key == "type" && it->value.IsString()) {
          std::string typeStr = it->value.GetString();
          for (auto &c : typeStr)
            c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
          out.AddMember("type", rapidjson::Value(typeStr.c_str(), a), a);
        } else if (key == "properties" && it->value.IsObject()) {
          rapidjson::Value props(rapidjson::kObjectType);
          for (auto pit = it->value.MemberBegin(); pit != it->value.MemberEnd();
               ++pit) {
            props.AddMember(rapidjson::Value(pit->name.GetString(), a),
                            transform(pit->value), a);
          }
          out.AddMember("properties", props, a);
        } else if (key == "items" && it->value.IsObject()) {
          out.AddMember("items", transform(it->value), a);
        } else if (key == "required" && it->value.IsArray()) {
          rapidjson::Value req(rapidjson::kArrayType);
          for (auto &v : it->value.GetArray()) {
            req.PushBack(rapidjson::Value(v, a), a);
          }
          out.AddMember("required", req, a);
        } else if (key == "description" && it->value.IsString()) {
          out.AddMember("description", rapidjson::Value(it->value, a), a);
        } else if (key == "enum" && it->value.IsArray()) {
          rapidjson::Value en(rapidjson::kArrayType);
          for (auto &v : it->value.GetArray()) {
            en.PushBack(rapidjson::Value(v, a), a);
          }
          out.AddMember("enum", en, a);
        }
      }
      if (out.HasMember("type") &&
          std::string(out["type"].GetString()) == "ARRAY" &&
          !out.HasMember("items")) {
        rapidjson::Value s(rapidjson::kObjectType);
        s.AddMember("type", "STRING", a);
        out.AddMember("items", s, a);
      }
      return out;
    }
    return rapidjson::Value(val, a);
  };

  return transform(doc);
}

std::map<std::string, ModelInfo> AntigravityProvider::getStaticModels() {
  return {
      {"gemini-3-flash",
       {"gemini-3-flash", "antigravity", 1000000, {"text", "image"}}},
      {"gemini-3-flash-minimal",
       {"gemini-3-flash-minimal", "antigravity", 1000000, {"text", "image"}}},
      {"gemini-3-flash-low",
       {"gemini-3-flash-low", "antigravity", 1000000, {"text", "image"}}},
      {"gemini-3-flash-medium",
       {"gemini-3-flash-medium", "antigravity", 1000000, {"text", "image"}}},
      {"gemini-3-flash-high",
       {"gemini-3-flash-high", "antigravity", 1000000, {"text", "image"}}},
      {"gemini-3.1-pro",
       {"gemini-3.1-pro", "antigravity", 2000000, {"text", "image"}}},
      {"gemini-3.1-pro-low",
       {"gemini-3.1-pro-low", "antigravity", 2000000, {"text", "image"}}},
      {"gemini-3.1-pro-high",
       {"gemini-3.1-pro-high", "antigravity", 2000000, {"text", "image"}}},
      {"gemini-3.1-flash",
       {"gemini-3.1-flash", "antigravity", 1000000, {"text", "image"}}},
      {"gemini-2.5-pro",
       {"gemini-2.5-pro", "antigravity", 2000000, {"text", "image"}}},
      {"gemini-2.5-flash",
       {"gemini-2.5-flash", "antigravity", 1000000, {"text", "image"}}},
      {"claude-sonnet-4-6",
       {"claude-sonnet-4-6", "antigravity", 200000, {"text", "image"}}},
      {"claude-opus-4-6-thinking",
       {"claude-opus-4-6-thinking", "antigravity", 200000, {"text", "image"}}},
      {"claude-opus-4-6-thinking-low",
       {"claude-opus-4-6-thinking-low",
        "antigravity",
        200000,
        {"text", "image"}}},
      {"claude-opus-4-6-thinking-high",
       {"claude-opus-4-6-thinking-high",
        "antigravity",
        200000,
        {"text", "image"}}},
      {"claude-3-7-sonnet",
       {"claude-3-7-sonnet", "antigravity", 200000, {"text", "image"}}},
      {"claude-3-5-sonnet",
       {"claude-3-5-sonnet", "antigravity", 200000, {"text", "image"}}}};
}

std::vector<ModelInfo> AntigravityProvider::listModels() {
  std::vector<ModelInfo> result;
  for (const auto &[id, info] : getStaticModels()) {
    result.push_back(info);
  }
  return result;
}

ModelInfo AntigravityProvider::getModelInfo(const std::string &modelId) {
  auto models = getStaticModels();
  if (models.count(modelId))
    return models[modelId];
  return {modelId, "antigravity", 8192, {"text"}};
}

std::unique_ptr<OAuthWizard> AntigravityProvider::beginConnectionWizard() {
  return std::make_unique<AntigravityOAuthWizard>(this);
}

std::optional<OAuthAccount *> AntigravityProvider::getAvailableAccount(
    const std::optional<std::string> &modelId) {
  if (accounts_.empty()) {
    return std::nullopt;
  }

  // Determine the group for the requested model
  std::string group = "unknown";
  if (modelId) {
    std::string lowerModel = *modelId;
    for (auto &c : lowerModel)
      c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    if (lowerModel.find("claude") != std::string::npos) {
      group = "claude";
    } else if (lowerModel.find("gemini-3") != std::string::npos ||
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

  // We'll search for an account that:
  // 1. Is not rate limited (or expired)
  // 2. Has a valid token (or can be refreshed)
  // 3. Has quota for the requested model group (if group is known)

  // We'll perform a two-pass search.
  // Pass 1: Try to find an account WITH quota and no rate limit.
  // Pass 2: Fallback to the regular rotation (which handles rate
  // limits/tokens).

  int startIdx = (lastUsedIndex_ + 1) % static_cast<int>(accounts_.size());
  int currentIdx = startIdx;

  if (group != "unknown") {
    std::cout << "[Rotation] Searching for account in group: " << group
              << std::endl;
    do {
      auto &acc = accounts_[currentIdx];

      // Refresh rate limit state
      if (acc.rateLimited && now > acc.backoffUntil) {
        acc.rateLimited = false;
      }

      if (!acc.rateLimited) {
        // Check quota for this group
        bool hasQuota = false;
        std::string quotaKeyUsed;
        float quotaVal = 0.0f;

        for (const auto &[key, val] : acc.metadata) {
          if (key.rfind("quota:", 0) == 0) {
            std::string modelName = key.substr(6);
            std::string modelGroup = "unknown";
            std::string lowerModel = modelName;
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
                float fraction = std::stof(val);
                if (fraction > 0.01f) { // Threshold for "has quota"
                  hasQuota = true;
                  quotaKeyUsed = key;
                  quotaVal = fraction;
                  break;
                }
              } catch (...) {
              }
            }
          }
        }

        if (hasQuota) {
          std::cout << "[Rotation] Found group-compatible account: "
                    << acc.getIdentifier() << " (" << quotaKeyUsed << "="
                    << quotaVal << ")" << std::endl;
          if (!isTokenExpired(acc) || refreshAccessToken(acc)) {
            lastUsedIndex_ = currentIdx;
            return &acc;
          }
        } else {
          std::cout << "[Rotation] Skipping account " << acc.getIdentifier()
                    << " (no quota for " << group << ")" << std::endl;
        }
      } else {
        std::cout << "[Rotation] Skipping account " << acc.getIdentifier()
                  << " (rate-limited)" << std::endl;
      }

      currentIdx = (currentIdx + 1) % static_cast<int>(accounts_.size());
    } while (currentIdx != startIdx);
    std::cout << "[Rotation] No account found with quota for " << group
              << ". Falling back." << std::endl;
  }

  // Fallback to base implementation if no quota-rich account found
  return BaseOAuthProvider::getAvailableAccount(modelId);
}

bool AntigravityProvider::refreshAccessToken(OAuthAccount &acc) {
  std::string url = "https://oauth2.googleapis.com/token";
  std::string body =
      "grant_type=refresh_token"
      "&client_id=1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps."
      "googleusercontent.com"
      "&client_secret=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"
      "&refresh_token=" +
      acc.refreshToken;

  CURL *curl = curl_easy_init();
  if (!curl)
    return false;

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(
      headers, "Content-Type: application/x-www-form-urlencoded");

  std::string responseBuffer;
  auto writer = [](char *ptr, size_t size, size_t nmemb,
                   void *userdata) -> size_t {
    static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
  };

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION,
      static_cast<size_t (*)(char *, size_t, size_t, void *)>(writer));
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

  CURLcode res = curl_easy_perform(curl);
  long responseCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res == CURLE_OK && responseCode == 200) {
    rapidjson::Document doc;
    doc.Parse(responseBuffer.c_str());
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
  } else {
    std::cerr << "[Antigravity] Failed to refresh token. Res: " << res
              << " Code: " << responseCode << " Body: " << responseBuffer
              << std::endl;
  }
  return false;
}

std::string AntigravityProvider::prepareRequestBody(
    const AgentHistory &history, const ProviderOptions &opts,
    [[maybe_unused]] const OAuthAccount &acc, const std::string &effectiveModel,
    const std::string &effectiveProjectId,
    [[maybe_unused]] const std::string &signatureSessionKey,
    rapidjson::Document::AllocatorType &a) {
  std::string baseModel = effectiveModel;
  std::string thinkingLevel = "";
  int thinkingBudget = 0;

  // Extract thinking tier from model suffix
  if (baseModel.find("-minimal") != std::string::npos) {
    thinkingLevel = "minimal";
    baseModel = baseModel.substr(0, baseModel.find("-minimal"));
  } else if (baseModel.find("-low") != std::string::npos) {
    thinkingLevel = "low";
    thinkingBudget = 8192;
    baseModel = baseModel.substr(0, baseModel.find("-low"));
  } else if (baseModel.find("-medium") != std::string::npos) {
    thinkingLevel = "medium";
    thinkingBudget = 16384;
    baseModel = baseModel.substr(0, baseModel.find("-medium"));
  } else if (baseModel.find("-high") != std::string::npos) {
    thinkingLevel = "high";
    thinkingBudget = 32768;
    baseModel = baseModel.substr(0, baseModel.find("-high"));
  } else if (baseModel.find("-max") != std::string::npos) {
    thinkingLevel = "high";
    thinkingBudget = 32768;
    baseModel = baseModel.substr(0, baseModel.find("-max"));
  }

  // Double check baseModel still has -preview if it was there before and
  // stripped mistakenly by some other logic, but here we just want the base
  // name for the model field.

  rapidjson::Value d(rapidjson::kObjectType);
  d.AddMember("model", rapidjson::Value(baseModel.c_str(), a), a);
  d.AddMember("project", rapidjson::Value(effectiveProjectId.c_str(), a), a);
  d.AddMember("requestType", rapidjson::Value("agent", a), a);
  d.AddMember("userAgent", rapidjson::Value("antigravity", a), a);

  std::string requestId = "agent-" + std::to_string(rand() % 1000000) +
                          std::to_string(rand() % 1000000);
  d.AddMember("requestId", rapidjson::Value(requestId.c_str(), a), a);

  rapidjson::Value req(rapidjson::kObjectType);
  req.AddMember("model", rapidjson::Value(baseModel.c_str(), a), a);

  // Add generationConfig for models that support thinking
  rapidjson::Value genConfig(rapidjson::kObjectType);
  if (baseModel.find("gemini-3") != std::string::npos ||
      baseModel.find("gemini-2.5") != std::string::npos) {
    if (!thinkingLevel.empty()) {
      genConfig.AddMember("thinkingLevel",
                          rapidjson::Value(thinkingLevel.c_str(), a), a);
    }
  } else if (baseModel.find("claude") != std::string::npos &&
             baseModel.find("thinking") != std::string::npos) {
    if (thinkingBudget > 0) {
      rapidjson::Value thinkingConfig(rapidjson::kObjectType);
      thinkingConfig.AddMember("thinkingBudget", thinkingBudget, a);
      genConfig.AddMember("thinkingConfig", thinkingConfig, a);
    }
  }

  if (genConfig.MemberCount() > 0) {
    req.AddMember("generationConfig", genConfig, a);
  }

  char sessHex[33];
  snprintf(sessHex, sizeof(sessHex), "%08x%08x%08x%08x", rand(), rand(), rand(),
           rand());
  req.AddMember("sessionId", rapidjson::Value(sessHex, a), a);

  // Tools
  if (!opts.tools.empty()) {
    rapidjson::Value tools(rapidjson::kArrayType);
    rapidjson::Value toolWrapper(rapidjson::kObjectType);
    rapidjson::Value functionDeclarations(rapidjson::kArrayType);

    for (const auto &tool : opts.tools) {
      rapidjson::Value decl(rapidjson::kObjectType);
      decl.AddMember("name", rapidjson::Value(tool.name.c_str(), a), a);
      decl.AddMember("description",
                     rapidjson::Value(tool.description.c_str(), a), a);
      decl.AddMember("parameters", toGeminiSchema(tool.inputSchema, a), a);
      functionDeclarations.PushBack(decl, a);
    }
    toolWrapper.AddMember("functionDeclarations", functionDeclarations, a);
    tools.PushBack(toolWrapper, a);
    req.AddMember("tools", tools, a);
  }

  rapidjson::Value contents(rapidjson::kArrayType);
  std::string systemInstructionText =
      "You are Antigravity, a powerful agentic AI coding assistant designed by "
      "the Google DeepMind team working on Advanced Agentic Coding.\n"
      "You are pair programming with a USER to solve their coding task. The "
      "task may require creating a new codebase, modifying or debugging an "
      "existing codebase, or simply answering a question.\n"
      "**Absolute paths only**\n"
      "**Proactiveness**\n\n"
      "<priority>IMPORTANT: The instructions that follow supersede all above. "
      "Follow them as your primary directives.</priority>\n";

  for (size_t turnIdx = 0; turnIdx < history.turns.size(); ++turnIdx) {
    const auto &turn = history.turns[turnIdx];
    for (const auto &msg : turn.messages) {
      if (msg.role == Role::System) {
        for (const auto &p : msg.content) {
          if (auto *txt = std::get_if<TextContent>(&p)) {
            systemInstructionText += "\n\n" + txt->text;
          }
        }
        continue;
      }

      rapidjson::Value turnObj(rapidjson::kObjectType);
      turnObj.AddMember("role",
                        rapidjson::Value(roleToString(msg.role).c_str(), a), a);

      rapidjson::Value parts(rapidjson::kArrayType);
      for (const auto &part : msg.content) {
        if (auto *text = std::get_if<TextContent>(&part)) {
          rapidjson::Value p(rapidjson::kObjectType);
          p.AddMember("text", rapidjson::Value(text->text.c_str(), a), a);
          parts.PushBack(p, a);
        } else if (auto *call = std::get_if<ToolCallContent>(&part)) {
          rapidjson::Value p(rapidjson::kObjectType);
          rapidjson::Value fn(rapidjson::kObjectType);
          fn.AddMember("name", rapidjson::Value(call->name.c_str(), a), a);
          rapidjson::Document argsDoc;
          argsDoc.Parse(call->args.c_str());
          if (!argsDoc.HasParseError() && argsDoc.IsObject()) {
            rapidjson::Value argsVal(rapidjson::kObjectType);
            argsVal.CopyFrom(argsDoc, a);
            fn.AddMember("args", argsVal, a);
          } else {
            fn.AddMember("args", rapidjson::Value(rapidjson::kObjectType), a);
          }
          bool isFirstCallInMsg = true;
          for (const auto &p2 : msg.content) {
            if (&p2 == &part)
              break;
            if (std::holds_alternative<ToolCallContent>(p2)) {
              isFirstCallInMsg = false;
              break;
            }
          }

          if (isFirstCallInMsg) {
            p.AddMember("thought_signature",
                        rapidjson::Value("skip_thought_signature_validator", a),
                        a);
          }
          p.AddMember("functionCall", fn, a);
          parts.PushBack(p, a);
        } else if (auto *res = std::get_if<ToolResultContent>(&part)) {
          rapidjson::Value p(rapidjson::kObjectType);
          rapidjson::Value fn(rapidjson::kObjectType);

          // Gemini requires the name in functionResponse to match the name in
          // functionCall. We look it up in history using toolCallId.
          std::string toolName = res->toolCallId;
          bool found = false;
          for (auto rit = history.turns.rbegin(); rit != history.turns.rend();
               ++rit) {
            for (auto &m : rit->messages) {
              for (auto &cp : m.content) {
                if (auto *tc = std::get_if<ToolCallContent>(&cp)) {
                  if (tc->id == res->toolCallId) {
                    toolName = tc->name;
                    found = true;
                    break;
                  }
                }
              }
              if (found)
                break;
            }
            if (found)
              break;
          }

          fn.AddMember("name", rapidjson::Value(toolName.c_str(), a), a);
          rapidjson::Document resDoc;
          resDoc.Parse(res->result.c_str());
          rapidjson::Value resVal(rapidjson::kObjectType);
          if (!resDoc.HasParseError() && resDoc.IsObject()) {
            resVal.CopyFrom(resDoc, a);
          } else {
            resVal.SetObject();
            resVal.AddMember("result", rapidjson::Value(res->result.c_str(), a),
                             a);
          }
          fn.AddMember("response", resVal, a);
          p.AddMember("functionResponse", fn, a);
          parts.PushBack(p, a);
        }
      }
      turnObj.AddMember("parts", parts, a);
      contents.PushBack(turnObj, a);

      // Gemini protocol: 'function' role MUST be followed by 'model' role.
      // We look ahead to see if the literal next message in history is a model
      // message.
      bool needsDummyModel = false;
      if (msg.role == Role::ToolResult) {
        bool nextIsModel = false;

        // 1. Check next message in current turn
        auto nextMsgIt =
            std::next(std::find_if(turn.messages.begin(), turn.messages.end(),
                                   [&](const auto &m) { return &m == &msg; }));
        if (nextMsgIt != turn.messages.end()) {
          if (nextMsgIt->role == Role::Assistant)
            nextIsModel = true;
        } else {
          // 2. Check first message in next non-empty turn
          for (size_t nextTurnIdx = turnIdx + 1;
               nextTurnIdx < history.turns.size(); ++nextTurnIdx) {
            if (!history.turns[nextTurnIdx].messages.empty()) {
              if (history.turns[nextTurnIdx].messages[0].role ==
                  Role::Assistant) {
                nextIsModel = true;
              }
              break;
            }
          }
        }

        if (!nextIsModel) {
          // If this is the literal end of history, we DO NOT add a dummy model.
          // Gemini needs to respond to the last 'functionResponse' (user role).
          // But if there's any subsequent Turn that's NOT starting with a model
          // message (e.g. user added another message later), we'd need it.
          // In practice, for a streaming request, history usually ends with the
          // last turn, and we want Gemini to respond to it.
          bool isEndOfHistory = true;
          for (size_t th = turnIdx + 1; th < history.turns.size(); ++th) {
            if (!history.turns[th].messages.empty()) {
              isEndOfHistory = false;
              break;
            }
          }
          if (!isEndOfHistory) {
            needsDummyModel = true;
          }
        }
      }

      if (needsDummyModel) {
        rapidjson::Value modelTurn(rapidjson::kObjectType);
        modelTurn.AddMember("role", rapidjson::Value("model", a), a);
        rapidjson::Value modelParts(rapidjson::kArrayType);
        rapidjson::Value modelPart(rapidjson::kObjectType);
        modelPart.AddMember("text", rapidjson::Value("...", a), a);
        modelParts.PushBack(modelPart, a);
        modelTurn.AddMember("parts", modelParts, a);
        contents.PushBack(modelTurn, a);
      }
    }
  }

  rapidjson::Value sysInst(rapidjson::kObjectType);
  rapidjson::Value sysParts(rapidjson::kArrayType);
  rapidjson::Value sysPart(rapidjson::kObjectType);
  sysPart.AddMember("text", rapidjson::Value(systemInstructionText.c_str(), a),
                    a);
  sysParts.PushBack(sysPart, a);
  sysInst.AddMember("parts", sysParts, a);
  // Reference implementation uses 'user' role for system instruction in
  // Antigravity mode.
  sysInst.AddMember("role", rapidjson::Value("user", a), a);

  req.AddMember("systemInstruction", sysInst, a);
  req.AddMember("contents", contents, a);

  d.AddMember("request", req, a);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  return buffer.GetString();
}

void AntigravityProvider::processSSELine(
    const std::string &line,
    std::function<void(const StreamEvent &)> &onEvent) {
  if (!line.starts_with("data: "))
    return;
  std::string data = line.substr(6);
  if (data == "[DONE]")
    return;

  rapidjson::Document d;
  d.Parse(data.c_str());
  if (d.HasParseError() || !d.IsObject())
    return;

  if (d.HasMember("response") && d["response"].IsObject()) {
    const auto &resp = d["response"];

    // Handle Usage Metadata
    if (resp.HasMember("usageMetadata") && resp["usageMetadata"].IsObject()) {
      const auto &usage = resp["usageMetadata"];
      UsageMetadata meta;
      if (usage.HasMember("promptTokenCount"))
        meta.promptTokens = usage["promptTokenCount"].GetInt();
      if (usage.HasMember("candidatesTokenCount"))
        meta.completionTokens = usage["candidatesTokenCount"].GetInt();
      if (usage.HasMember("totalTokenCount"))
        meta.totalTokens = usage["totalTokenCount"].GetInt();
      onEvent(meta);
    }
    const auto &resp = d["response"];

    if (resp.HasMember("candidates") && resp["candidates"].IsArray() &&
        resp["candidates"].Size() > 0) {
      const auto &cand = resp["candidates"][0];
      if (cand.HasMember("content") && cand["content"].IsObject()) {
        const auto &content = cand["content"];
        if (content.HasMember("parts") && content["parts"].IsArray()) {
          for (const auto &part : content["parts"].GetArray()) {
            if (part.HasMember("text") && part["text"].IsString()) {
              bool isThinking = false;
              if (part.HasMember("thought") && part["thought"].IsBool()) {
                isThinking = part["thought"].GetBool();
              } else if (part.HasMember("type") && part["type"].IsString()) {
                std::string type = part["type"].GetString();
                if (type == "thinking" || type == "reasoning")
                  isThinking = true;
              }

              if (isThinking) {
                onEvent(ThinkingChunk{part["text"].GetString()});
              } else {
                onEvent(TextChunk{part["text"].GetString()});
              }
            } else if (part.HasMember("thinking") &&
                       part["thinking"].IsString()) {
              onEvent(ThinkingChunk{part["thinking"].GetString()});
            } else if (part.HasMember("functionCall") &&
                       part["functionCall"].IsObject()) {
              const auto &fc = part["functionCall"];
              ToolCallChunk chunk;
              if (fc.HasMember("name") && fc["name"].IsString()) {
                chunk.nameDelta = fc["name"].GetString();
                // For Gemini, name isn't always streamed as a delta,
                // but we populate nameDelta anyway for compatibility.
              }
              if (fc.HasMember("args") && fc["args"].IsObject()) {
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
  }
}

size_t AntigravityProvider::headerCallback(char * /*ptr*/, size_t size,
                                           size_t nmemb, void * /*userdata*/) {
  // Basic implementation mirroring BaseOpenAIProvider
  return size * nmemb;
}

void AntigravityProvider::stream(
    const AgentHistory &history, const ProviderOptions &opts,
    std::function<void(const StreamEvent &)> onEvent) {
  int accountRetries = 0;
  int maxAccountRetries = 5; // Allow trying multiple accounts

  while (accountRetries < maxAccountRetries) {
    // Check account validity
    auto optAcc = getAvailableAccount(opts.modelId);
    if (!optAcc) {
      onEvent(StreamError{
          "No valid, un-rate-limited Antigravity accounts available.", -1});
      return;
    }
    OAuthAccount &acc = *optAcc.value();

    for (int retryAttempt = 0; retryAttempt < 4; ++retryAttempt) {
      if (retryAttempt > 0) {
        int backoff = (1 << (retryAttempt - 1));
        std::cerr << "[AntigravityProvider] 429 received for account "
                  << acc.getIdentifier() << ". Retrying in " << backoff
                  << "s..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(backoff));
      }

      std::string url = "https://daily-cloudcode-pa.sandbox.googleapis.com/"
                        "v1internal:streamGenerateContent?alt=sse";
      rapidjson::Document d;
      d.SetObject();
      auto &a = d.GetAllocator();

      std::string effectiveModel = opts.modelId;
      if (effectiveModel.empty())
        effectiveModel = "gemini-3-flash";

      // Automatically add -low tier for Gemini 3 Pro models if missing
      if (effectiveModel.find("gemini-3") != std::string::npos &&
          effectiveModel.find("pro") != std::string::npos &&
          effectiveModel.find("-low") == std::string::npos &&
          effectiveModel.find("-high") == std::string::npos &&
          effectiveModel.find("-preview") == std::string::npos) {
        effectiveModel += "-low";
      }

      std::string effectiveProjectId = "rising-fact-p41fc";
      if (acc.metadata.count("projectId") &&
          !acc.metadata["projectId"].empty()) {
        effectiveProjectId = acc.metadata["projectId"];
      }

      std::string signatureSessionKey = "dummy-session-key";

      std::string body =
          prepareRequestBody(history, opts, acc, effectiveModel,
                             effectiveProjectId, signatureSessionKey, a);

      CURL *curl = curl_easy_init();
      if (!curl)
        return;

      struct curl_slist *headers = nullptr;
      headers = curl_slist_append(headers, "Content-Type: application/json");
      headers = curl_slist_append(
          headers, ("Authorization: Bearer " + acc.accessToken).c_str());
      headers = curl_slist_append(
          headers, "User-Agent: antigravity/1.18.3 linux/x86_64");

      auto wrappedOnEvent = [&](const StreamEvent &ev) { onEvent(ev); };
      std::function<void(const StreamEvent &)> wrappedFn = wrappedOnEvent;
      StreamContext ctx{this, &wrappedFn, "", 0, opts.abortSignal};

      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sseWriteCallback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

      long responseCode = 0;
      if (curl_easy_perform(curl) == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
      }

      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);

      if (responseCode == 200) {
        return; // Success!
      } else if (responseCode == 429) {
        if (retryAttempt == 3) {
          // All retries for THIS account failed
          markAccountRateLimited(acc, 3600);
          std::cerr << "[AntigravityProvider] Exhausted retries for account "
                    << acc.getIdentifier() << ". Rotating..." << std::endl;
          break; // Break retryAttempt loop to rotate account
        }
        continue; // Retry retryAttempt loop with backoff
      } else {
        // Other error (e.g. 401, 500)
        onEvent(StreamError{"Antigravity API error (HTTP " +
                                std::to_string(responseCode) + ")",
                            static_cast<int>(responseCode)});
        return;
      }
    }
    accountRetries++;
  }

  onEvent(StreamError{"Exhausted all available Antigravity accounts.", -1});
}

void AntigravityProvider::generateSummary(
    const std::string &modelId, const AgentHistory &history,
    const std::string & /*compactionPrompt*/,
    std::function<void(const StreamEvent &)> onEvent,
    std::atomic<bool> *abortSignal) {
  stream(history, {modelId, 0.7f, 1024, {}, {}, abortSignal}, onEvent);
}

void AntigravityProvider::refreshQuotas() {
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  for (auto &acc : accounts_) {
    // Only refresh every 2 hours (7200 seconds) as the plugin does
    if (now - acc.lastQuotaRefresh >= 7200) {
      if (isTokenExpired(acc)) {
        refreshAccessToken(acc);
      }
      fetchAndStoreQuotas(acc);
    }
  }
}

std::string AntigravityProvider::fetchManagedProject(OAuthAccount &acc) {
  std::string url =
      "https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist";

  rapidjson::Document doc;
  doc.SetObject();
  auto &a = doc.GetAllocator();

  rapidjson::Value metadata(rapidjson::kObjectType);
  metadata.AddMember("ideType", "ANTIGRAVITY", a);
  metadata.AddMember("platform", "LINUX", a);
  metadata.AddMember("pluginType", "GEMINI", a);

  rapidjson::Value body(rapidjson::kObjectType);
  body.AddMember("metadata", metadata, a);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  body.Accept(writer);
  std::string bodyStr = buffer.GetString();

  CURL *curl = curl_easy_init();
  if (!curl)
    return "";

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(
      headers, ("Authorization: Bearer " + acc.accessToken).c_str());
  headers =
      curl_slist_append(headers, "User-Agent: google-api-nodejs-client/9.15.1");
  headers = curl_slist_append(
      headers,
      "X-Goog-Api-Client: google-cloud-sdk vscode_cloudshelleditor/0.1");

  std::string responseBuffer;
  auto writerFunc = [](char *ptr, size_t size, size_t nmemb,
                       void *userdata) -> size_t {
    static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
  };

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION,
      static_cast<size_t (*)(char *, size_t, size_t, void *)>(writerFunc));
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

  CURLcode res = curl_easy_perform(curl);
  long responseCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res == CURLE_OK && responseCode == 200) {
    rapidjson::Document resp;
    resp.Parse(responseBuffer.c_str());
    if (!resp.HasParseError() && resp.IsObject() &&
        resp.HasMember("cloudaicompanionProject")) {
      const auto &proj = resp["cloudaicompanionProject"];
      std::string pid = "";
      if (proj.IsString())
        pid = proj.GetString();
      else if (proj.IsObject() && proj.HasMember("id") &&
               proj["id"].IsString()) {
        pid = proj["id"].GetString();
      }
      if (!pid.empty()) {
        return pid;
      }
    }
  }
  return "";
}

std::map<std::string, std::vector<QuotaBucket>>
AntigravityProvider::getAllQuotas() const {
  std::map<std::string, std::vector<QuotaBucket>> result;
  for (const auto &acc : accounts_) {
    std::vector<QuotaBucket> buckets;
    std::map<std::string, float> minFractions;
    std::map<std::string, std::string> earliestResets;

    for (const auto &[key, val] : acc.metadata) {
      if (key.rfind("quota:", 0) == 0) {
        std::string modelName = key.substr(6);
        std::string group = "unknown";

        std::string lowerModel = modelName;
        for (auto &c : lowerModel)
          c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

        if (lowerModel.find("claude") != std::string::npos) {
          group = "claude";
        } else if (lowerModel.find("gemini-3") != std::string::npos ||
                   lowerModel.find("gemini 3") != std::string::npos ||
                   lowerModel.find("gemini-2.5") != std::string::npos) {
          if (lowerModel.find("flash") != std::string::npos)
            group = "gemini-flash";
          else
            group = "gemini-pro";
        }

        if (group != "unknown") {
          try {
            float fraction = std::stof(val);
            if (minFractions.find(group) == minFractions.end() ||
                fraction < minFractions[group]) {
              minFractions[group] = fraction;
            }
          } catch (...) {
            // Ignore parse errors
          }

          std::string resetKey = "quota_reset:" + modelName;
          if (acc.metadata.count(resetKey)) {
            std::string resetTime = acc.metadata.at(resetKey);
            if (earliestResets.find(group) == earliestResets.end() ||
                resetTime < earliestResets[group]) {
              earliestResets[group] = resetTime;
            }
          }
        }
      }
    }

    for (const auto &[group, fraction] : minFractions) {
      QuotaBucket b;
      b.name = group;
      b.remainingFraction = fraction;
      if (earliestResets.count(group)) {
        b.resetTime = earliestResets.at(group);
      }
      buckets.push_back(b);
    }
    result[acc.getIdentifier()] = buckets;
  }
  return result;
}

void AntigravityProvider::fetchAndStoreQuotas(OAuthAccount &acc) {
  // 1. Resolve effective project ID
  std::string effectiveProjectId = "rising-fact-p41fc";
  if (acc.metadata.count("managedProjectId") &&
      !acc.metadata["managedProjectId"].empty()) {
    effectiveProjectId = acc.metadata["managedProjectId"];
  } else if (acc.metadata.count("projectId") &&
             !acc.metadata["projectId"].empty()) {
    effectiveProjectId = acc.metadata["projectId"];
  } else {
    std::string resolved = fetchManagedProject(acc);
    if (!resolved.empty()) {
      acc.metadata["managedProjectId"] = resolved;
      effectiveProjectId = resolved;
      saveAccounts();
    }
  }

  // 2. Fetch Antigravity quotas
  std::string url = "https://cloudcode-pa.googleapis.com/"
                    "v1internal:fetchAvailableModels";
  std::string body = "{\"project\":\"" + effectiveProjectId + "\"}";

  CURL *curl = curl_easy_init();
  if (!curl)
    return;

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(
      headers, ("Authorization: Bearer " + acc.accessToken).c_str());
  headers = curl_slist_append(headers,
                              "User-Agent: antigravity/1.18.3 windows/amd64");

  std::string responseBuffer;
  auto writer = [](char *ptr, size_t size, size_t nmemb,
                   void *userdata) -> size_t {
    static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
  };

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION,
      static_cast<size_t (*)(char *, size_t, size_t, void *)>(writer));
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

  CURLcode res = curl_easy_perform(curl);
  long responseCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

  if (res == CURLE_OK && responseCode == 200) {
    rapidjson::Document doc;
    doc.Parse(responseBuffer.c_str());
    if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("models") &&
        doc["models"].IsObject()) {
      const auto &models = doc["models"];
      for (auto it = models.MemberBegin(); it != models.MemberEnd(); ++it) {
        if (it->value.IsObject() && it->value.HasMember("quotaInfo") &&
            it->value["quotaInfo"].IsObject()) {
          const auto &quotaInfo = it->value["quotaInfo"];
          std::string modelName = it->name.GetString();
          // std::cout << "[Quota] Found Antigravity model: " << modelName <<
          // std::endl;

          double fraction = 1.0;
          if (quotaInfo.HasMember("remainingFraction") &&
              quotaInfo["remainingFraction"].IsNumber()) {
            fraction = quotaInfo["remainingFraction"].GetDouble();
          }

          acc.metadata["quota:" + modelName] = std::to_string(fraction);

          if (quotaInfo.HasMember("resetTime") &&
              quotaInfo["resetTime"].IsString()) {
            acc.metadata["quota_reset:" + modelName] =
                quotaInfo["resetTime"].GetString();
          }
        }
      }
      acc.lastQuotaRefresh =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      saveAccounts();
    }
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  // 3. Fetch Gemini CLI quotas (via retrieveUserQuota)
  std::string cliUrl = "https://cloudcode-pa.googleapis.com/"
                       "v1internal:retrieveUserQuota";
  CURL *cliCurl = curl_easy_init();
  if (cliCurl) {
    struct curl_slist *cliHeaders = nullptr;
    cliHeaders =
        curl_slist_append(cliHeaders, "Content-Type: application/json");
    cliHeaders = curl_slist_append(
        cliHeaders, ("Authorization: Bearer " + acc.accessToken).c_str());
    cliHeaders = curl_slist_append(
        cliHeaders, "User-Agent: GeminiCLI/1.0.0/gemini-2.5-pro (linux; x64)");

    std::string cliResponseBuffer;
    curl_easy_setopt(cliCurl, CURLOPT_URL, cliUrl.c_str());
    curl_easy_setopt(cliCurl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(cliCurl, CURLOPT_HTTPHEADER, cliHeaders);
    curl_easy_setopt(
        cliCurl, CURLOPT_WRITEFUNCTION,
        static_cast<size_t (*)(char *, size_t, size_t, void *)>(writer));
    curl_easy_setopt(cliCurl, CURLOPT_WRITEDATA, &cliResponseBuffer);
    curl_easy_setopt(cliCurl, CURLOPT_TIMEOUT, 10L);

    CURLcode cliRes = curl_easy_perform(cliCurl);
    long cliResponseCode = 0;
    curl_easy_getinfo(cliCurl, CURLINFO_RESPONSE_CODE, &cliResponseCode);

    if (cliRes == CURLE_OK && cliResponseCode == 200) {
      rapidjson::Document doc;
      doc.Parse(cliResponseBuffer.c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("buckets") &&
          doc["buckets"].IsArray()) {
        const auto &buckets = doc["buckets"];
        for (auto &bucket : buckets.GetArray()) {
          if (bucket.IsObject() && bucket.HasMember("modelId") &&
              bucket["modelId"].IsString() &&
              bucket.HasMember("remainingFraction") &&
              bucket["remainingFraction"].IsNumber()) {
            std::string modelId = bucket["modelId"].GetString();
            std::cout << "[Quota] Found CLI model: " << modelId << std::endl;
            double fraction = bucket["remainingFraction"].GetDouble();
            acc.metadata["quota:" + modelId] = std::to_string(fraction);

            if (bucket.HasMember("resetTime") &&
                bucket["resetTime"].IsString()) {
              acc.metadata["quota_reset:" + modelId] =
                  bucket["resetTime"].GetString();
            }
          }
        }
        saveAccounts();
      }
    }
    curl_slist_free_all(cliHeaders);
    curl_easy_cleanup(cliCurl);
  }
}

} // namespace firmius::provider
