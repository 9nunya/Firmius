#include "providers/KiloProvider.hpp"

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <sstream>
#include <atomic>
#include <thread>
#include <mutex>
#include <optional>
#include <chrono>
#include <algorithm>

namespace firmius::provider {

using namespace firmius::shared;

using namespace shared;

namespace {

constexpr const char *kKiloBaseUrl = "https://api.kilo.ai";
constexpr const char *kDeviceCodePath = "/api/device-auth/codes";
constexpr const char *kTokenPollPath = "/api/device-auth/codes/";
constexpr const char *kOpenrouterPath = "/api/openrouter";
constexpr const char *kUserAgent = "Firmius/1.0";
constexpr int64_t kDefaultBudgetMicrodollars = 1'000'000;

// Write callback for curl
size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

// ==================== KiloAPIKeyWizard ====================

KiloAPIKeyWizard::KiloAPIKeyWizard() : prompt_(""), isComplete_(false) {}

KiloAPIKeyWizard::~KiloAPIKeyWizard() { stopPolling(); }

std::optional<firmius::WizardPrompt> KiloAPIKeyWizard::nextPrompt() {
  if (!promptShown_) {
    promptShown_ = true;
    try {
      deviceResponse_ = requestDeviceCode();
      prompt_ =
          "Open this URL in a browser and authorize:\n\n" +
          deviceResponse_.verificationUrl +
          "\n\nEnter code: " + deviceResponse_.code + "\n"
          "Waiting for authorization...";
      startPolling();
    } catch (const std::exception &e) {
      errorMessage_ = "Failed to start device auth: " + std::string(e.what());
      isComplete_ = true;
      return std::nullopt;
    }
    firmius::WizardPrompt prompt;
    prompt.message = prompt_;
    prompt.allowFreeformInput = false;
    prompt.allowEmptyInput = true;
    prompt.submitLabel = "Open Browser / Wait";
    return prompt;
  }
  return std::nullopt;
}

void KiloAPIKeyWizard::submitAnswer(const std::string &) { /* unused */ }

bool KiloAPIKeyWizard::isComplete() const { return pollingDone_.load(); }

bool KiloAPIKeyWizard::finalizeExchange(std::string &outApiKey, std::string &outErrorMessage) {
  if (!errorMessage_.empty()) {
    outErrorMessage = errorMessage_;
    return false;
  }
  if (token_.empty()) {
    outErrorMessage = "Authorization was not completed.";
    return false;
  }
  outApiKey = token_;
  return true;
}

std::string KiloAPIKeyWizard::getFinalMessage() const {
  if (!errorMessage_.empty())
    return "Kilo authentication failed: " + errorMessage_;
  return "Successfully authenticated with Kilo!";
}

void KiloAPIKeyWizard::startPolling() {
  stopPolling_ = false;
  pollingThread_ = std::thread([this]() { pollLoop(); });
}

void KiloAPIKeyWizard::stopPolling() {
  stopPolling_ = true;
  if (pollingThread_.joinable())
    pollingThread_.join();
}

void KiloAPIKeyWizard::pollLoop() {
  pollingDone_ = false;
  auto tokenOpt = pollForToken(deviceResponse_.code, deviceResponse_.expiresIn);
  if (tokenOpt.has_value()) {
    token_ = tokenOpt->accessToken;
  } else {
    errorMessage_ = pollingError_.empty() ? "Authorization timed out or was denied." : pollingError_;
  }
  pollingDone_ = true;
}

KiloAPIKeyWizard::DeviceAuthResponse KiloAPIKeyWizard::requestDeviceCode() {
  CURL *curl = curl_easy_init();
  if (!curl)
    throw std::runtime_error("curl init failed");

  std::string url = std::string(kKiloBaseUrl) + kDeviceCodePath;
  std::string response;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, (std::string("User-Agent: ") + kUserAgent).c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  CURLcode res = curl_easy_perform(curl);
  long httpStatus = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    throw std::runtime_error("curl error: " + std::string(curl_easy_strerror(res)));
  }
  if (httpStatus != 200) {
    throw std::runtime_error("device code request failed: HTTP " + std::to_string(httpStatus) + " body: " + response);
  }

  rapidjson::Document d;
  d.Parse(response.c_str());
  if (d.HasParseError() || !d.IsObject())
    throw std::runtime_error("Invalid JSON response from device code endpoint");

  DeviceAuthResponse out;
  if (d.HasMember("code") && d["code"].IsString())
    out.code = d["code"].GetString();
  else
    throw std::runtime_error("Missing 'code' in response");

  if (d.HasMember("verification_url") && d["verification_url"].IsString())
    out.verificationUrl = d["verification_url"].GetString();
  else if (d.HasMember("verificationUrl") && d["verificationUrl"].IsString())
    out.verificationUrl = d["verificationUrl"].GetString();
  else
    throw std::runtime_error("Missing 'verification_url' in response");

  if (d.HasMember("expires_in") && d["expires_in"].IsInt())
    out.expiresIn = d["expires_in"].GetInt();
  else
    out.expiresIn = 600;

  return out;
}

std::optional<KiloAPIKeyWizard::TokenResponse>
KiloAPIKeyWizard::pollForToken(const std::string &code, int expiresIn) {
  const int pollIntervalMs = 3000;
  const int maxAttempts = (expiresIn * 1000) / pollIntervalMs;
  CURL *curl = curl_easy_init();
  if (!curl)
    return std::nullopt;

  std::string url = std::string(kKiloBaseUrl) + kTokenPollPath + code;

  for (int attempt = 0; attempt < maxAttempts; ++attempt) {
    if (stopPolling_.load())
      return std::nullopt;

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, (std::string("User-Agent: ") + kUserAgent).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

    if (res == CURLE_OK && httpStatus == 200) {
      rapidjson::Document d;
      d.Parse(response.c_str());
      if (!d.HasParseError() && d.IsObject()) {
        TokenResponse token;
        if (d.HasMember("token") && d["token"].IsString()) {
          token.accessToken = d["token"].GetString();
          if (d.HasMember("refresh_token") && d["refresh_token"].IsString())
            token.refreshToken = d["refresh_token"].GetString();
          if (d.HasMember("expires_in") && d["expires_in"].IsInt())
            token.expiresIn = d["expires_in"].GetInt();
          else
            token.expiresIn = expiresIn;
          curl_easy_cleanup(curl);
          return token;
        }
      }
    } else if (httpStatus == 403) {
      pollingError_ = "Authorization denied.";
      break;
    } else if (httpStatus == 410) {
      pollingError_ = "Authorization expired.";
      break;
    } else if (httpStatus == 202) {
      // still pending
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
  }

  curl_easy_cleanup(curl);
  return std::nullopt;
}

// ==================== KiloProvider ====================

KiloProvider::KiloProvider()
    : BaseOpenAIProvider("kilo", std::string(kKiloBaseUrl) + kOpenrouterPath, ""),
      baseUrl_(std::string(kKiloBaseUrl) + kOpenrouterPath) {}

KiloProvider::~KiloProvider() = default;

std::vector<ModelInfo> KiloProvider::listModels() {
  ensureModelsLoaded();
  std::lock_guard<std::mutex> lock(modelsMutex_);
  std::vector<ModelInfo> result;
  for (const auto &c : modelCache_) {
    ModelInfo mi;
    mi.id = c.id;
    mi.provider = "kilo";
    mi.contextWindow = c.contextWindow;
    mi.maxOutputTokens = c.maxOutputTokens;
    mi.modalities = {"text"};
    if (c.supportsImages)
      mi.modalities.push_back("image");
    mi.supportsReasoning = c.supportsReasoning;
    mi.pricePer1MInput = c.pricePer1MInput;
    mi.pricePer1MOutput = c.pricePer1MOutput;
    mi.pricePer1MCacheRead = c.pricePer1MCacheRead;
    mi.pricePer1MCacheWrite = c.pricePer1MCacheWrite;
    result.push_back(mi);
  }
  return result;
}

ModelInfo KiloProvider::getModelInfo(const std::string &modelId) {
  auto models = listModels();
  for (const auto &m : models) {
    if (m.id == modelId)
      return m;
  }
  ModelInfo mi;
  mi.id = modelId;
  mi.provider = "kilo";
  mi.contextWindow = 8192;
  mi.modalities = {"text"};
  return mi;
}

std::unique_ptr<APIKeyWizard> KiloProvider::beginConnectionWizard() {
  return std::make_unique<KiloAPIKeyWizard>();
}

std::optional<APIKeyAccount *> KiloProvider::getAvailableAccount(
    const std::optional<std::string> &modelId) {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);

  if (accounts_.empty()) {
    if (modelId.has_value() && isFreeModel(*modelId)) {
      getOrCreateAnonymousAccount();
      for (auto &acc : accounts_) {
        if (isAnonymous(acc)) return &acc;
      }
    }
    return std::nullopt;
  }

  int startIdx = lastUsedIndex_.load(std::memory_order_relaxed);
  if (startIdx < 0 || startIdx >= static_cast<int>(accounts_.size()))
    startIdx = 0;

  // Prefer non-anonymous, non-exhausted accounts
  for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
    int idx = (startIdx + i) % static_cast<int>(accounts_.size());
    auto &acc = accounts_[idx];
    if (isAnonymous(acc))
      continue;
    if (!isExhausted(acc, modelId)) {
      lastUsedIndex_.store(idx, std::memory_order_relaxed);
      return &acc;
    }
  }

  // All real accounts exhausted. Check anonymous fallback for free models.
  if (modelId.has_value() && isFreeModel(*modelId)) {
    auto it = std::find_if(accounts_.begin(), accounts_.end(),
                           [this](const auto &a) { return isAnonymous(a); });
    if (it != accounts_.end()) return &(*it);

    getOrCreateAnonymousAccount();
    for (auto &acc : accounts_) {
      if (isAnonymous(acc)) return &acc;
    }
  }

  return std::nullopt;
}

// --- Helpers ---

bool KiloProvider::isAnonymous(const APIKeyAccount &acc) const {
  return acc.apiKey == kAnonymousToken ||
         acc.metadata.find(kMetaAnonymousFlag) != acc.metadata.end();
}

bool KiloProvider::isExhausted(const APIKeyAccount &acc,
                               const std::optional<std::string> &/*modelId*/) const {
  if (acc.rateLimited)
    return true; // still rate limited

  auto budgetIt = acc.metadata.find(kMetaBudget);
  if (budgetIt != acc.metadata.end()) {
    try {
      int64_t budget = std::stoll(budgetIt->second);
      if (budget <= 0)
        return true;
      auto spentIt = acc.metadata.find(kMetaAccumulatedCost);
      int64_t spent = spentIt != acc.metadata.end() ? std::stoll(spentIt->second) : 0;
      if (spent >= budget)
        return true;
    } catch (...) {}
  }
  return false;
}

void KiloProvider::updateSpending(APIKeyAccount &acc, const std::string &modelId,
                                   std::uint64_t promptTokens,
                                   std::uint64_t completionTokens) {
  // Fetch prices WITHOUT holding accountsMutex_ to avoid deadlock.
  double inputPrice = getModelPricePer1MInput(modelId);
  double outputPrice = getModelPricePer1MOutput(modelId);

  int64_t addMicros =
      static_cast<int64_t>(inputPrice * promptTokens) +
      static_cast<int64_t>(outputPrice * completionTokens);

  // Now acquire accountsMutex_ only for the metadata update.
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  int64_t current = 0;
  auto it = acc.metadata.find(kMetaAccumulatedCost);
  if (it != acc.metadata.end()) {
    try { current = std::stoll(it->second); } catch(...) {}
  }
  acc.metadata[kMetaAccumulatedCost] = std::to_string(current + addMicros);
}

bool KiloProvider::isFreeModel(const std::string &modelId) const {
  return modelId.find(":free") != std::string::npos;
}

double KiloProvider::getModelPricePer1MInput(const std::string &modelId) {
  ensureModelsLoaded();
  std::lock_guard<std::mutex> lock(modelsMutex_);
  for (const auto &c : modelCache_) {
    if (c.id == modelId) return c.pricePer1MInput;
  }
  return 0.0;
}

double KiloProvider::getModelPricePer1MOutput(const std::string &modelId) {
  ensureModelsLoaded();
  std::lock_guard<std::mutex> lock(modelsMutex_);
  for (const auto &c : modelCache_) {
    if (c.id == modelId) return c.pricePer1MOutput;
  }
  return 0.0;
}

void KiloProvider::ensureModelsLoaded() {
  {
    std::lock_guard<std::mutex> lock(modelsMutex_);
    if (!modelCache_.empty())
      return;
  }

  // Get a token (may be anonymous) WITHOUT holding modelsMutex_.
  // This avoids deadlock: getAvailableAccount() locks accountsMutex_,
  // and we must not hold modelsMutex_ while acquiring it.
  std::string token;
  {
    std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
    if (!accounts_.empty()) {
      token = accounts_.front().apiKey;
    }
  }

  CURL *curl = curl_easy_init();
  if (!curl)
    return;

  std::string url = baseUrl_ + "/models";
  std::string response;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, ("User-Agent: " + std::string(kUserAgent)).c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!token.empty()) {
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  CURLcode res = curl_easy_perform(curl);
  long httpStatus = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || httpStatus != 200) {
    return;
  }

  rapidjson::Document d;
  d.Parse(response.c_str());
  if (!d.IsObject() || !d.HasMember("data") || !d["data"].IsArray())
    return;

  std::lock_guard<std::mutex> lock(modelsMutex_);
  for (const auto &m : d["data"].GetArray()) {
    if (!m.IsObject())
      continue;
    CachedModelInfo info;
    if (m.HasMember("id") && m["id"].IsString())
      info.id = m["id"].GetString();
    else
      continue;

    if (m.HasMember("context_length") && m["context_length"].IsUint())
      info.contextWindow = m["context_length"].GetUint();
    else if (m.HasMember("context_window") && m["context_window"].IsUint())
      info.contextWindow = m["context_window"].GetUint();

    if (m.HasMember("max_completion_tokens") && m["max_completion_tokens"].IsUint())
      info.maxOutputTokens = m["max_completion_tokens"].GetUint();

    if (m.HasMember("pricing") && m["pricing"].IsObject()) {
      const auto &p = m["pricing"];
      if (p.HasMember("prompt") && p["prompt"].IsString()) {
        try { info.pricePer1MInput = std::stod(p["prompt"].GetString()); } catch(...) {}
      }
      if (p.HasMember("completion") && p["completion"].IsString()) {
        try { info.pricePer1MOutput = std::stod(p["completion"].GetString()); } catch(...) {}
      }
      if (p.HasMember("input_cache_write") && p["input_cache_write"].IsString()) {
        try { info.pricePer1MCacheWrite = std::stod(p["input_cache_write"].GetString()); } catch(...) {}
      }
      if (p.HasMember("input_cache_read") && p["input_cache_read"].IsString()) {
        try { info.pricePer1MCacheRead = std::stod(p["input_cache_read"].GetString()); } catch(...) {}
      }
    }

    if (m.HasMember("supported_parameters") && m["supported_parameters"].IsArray()) {
      const auto &params = m["supported_parameters"];
      for (const auto &p : params.GetArray()) {
        if (p.IsString()) {
          std::string s = p.GetString();
          if (s == "tools") info.supportsTools = true;
          if (s == "reasoning") info.supportsReasoning = true;
          if (s == "temperature") info.supportsTemperature = true;
        }
      }
    }

    if (m.HasMember("architecture") && m["architecture"].IsObject()) {
      const auto &arch = m["architecture"];
      if (arch.HasMember("input_modalities") && arch["input_modalities"].IsArray()) {
        const auto &mods = arch["input_modalities"];
        for (const auto &mod : mods.GetArray()) {
          if (mod.IsString() && mod.GetString() == std::string("image"))
            info.supportsImages = true;
        }
      }
    }

    info.family = extractFamily(info.id);
    modelCache_.push_back(info);
  }
}

std::string KiloProvider::extractFamily(const std::string &modelId) const {
  size_t slash = modelId.find('/');
  if (slash == std::string::npos)
    return "";
  std::string namePart = modelId.substr(slash + 1);
  if (namePart.find("claude") != std::string::npos) return "claude";
  if (namePart.find("gpt") != std::string::npos || namePart.find("o1") != std::string::npos ||
      namePart.find("codex") != std::string::npos) return "openai";
  if (namePart.find("gemini") != std::string::npos) return "gemini";
  if (namePart.find("llama") != std::string::npos || namePart.find("mistral") != std::string::npos)
    return "open-source";
  if (namePart.find("qwen") != std::string::npos) return "qwen";
  if (modelId.find("kilo-auto") != std::string::npos) return "kilo-auto";
  return namePart.substr(0, namePart.find('-'));
}

APIKeyAccount &KiloProvider::getOrCreateAnonymousAccount() {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  for (auto &acc : accounts_) {
    if (isAnonymous(acc))
      return acc;
  }
  APIKeyAccount anon;
  anon.identifier = kAnonymousIdentifier;
  anon.keyPrefix = "";
  anon.apiKey = kAnonymousToken;
  anon.metadata[kMetaAnonymousFlag] = "true";
  anon.metadata[kMetaBudget] = std::to_string(kDefaultBudgetMicrodollars);
  accounts_.push_back(anon);
  return accounts_.back();
}

// --- Quota ---

void KiloProvider::refreshQuotas() {
  // Ensure models and pricing are loaded. Must be done without holding
  // accountsMutex_ to maintain lock order: modelsMutex_ -> accountsMutex_.
  ensureModelsLoaded();

  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  // No remote quota API for Kilo yet; cost is tracked locally in account metadata.
}

std::map<std::string, std::vector<QuotaBucket>> KiloProvider::getAllQuotas() const {
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  std::map<std::string, std::vector<QuotaBucket>> result;

  for (const auto &acc : accounts_) {
    std::vector<QuotaBucket> buckets;

    auto budgetIt = acc.metadata.find(kMetaBudget);
    int64_t budget = kDefaultBudgetMicrodollars;
    if (budgetIt != acc.metadata.end()) {
      try { budget = std::stoll(budgetIt->second); } catch(...) {}
    }

    auto spentIt = acc.metadata.find(kMetaAccumulatedCost);
    int64_t spent = spentIt != acc.metadata.end() ? std::stoll(spentIt->second) : 0;

    float remainingFraction = 1.0f;
    if (budget > 0) {
      remainingFraction = static_cast<float>(budget - spent) / static_cast<float>(budget);
      if (remainingFraction < 0.0f) remainingFraction = 0.0f;
    }

    std::string note;
    if (isAnonymous(acc)) {
      note = "Anonymous free tier — no charges tracked";
    } else if (budget > 0) {
      note = "Budget: $" + std::to_string(budget / 1'000'000.f) + " — Spent: $" +
             std::to_string(spent / 1'000'000.f);
    } else {
      note = "No budget limit set";
    }

    buckets.push_back(QuotaBucket{"kilo-spend", remainingFraction, "", note});
    result[acc.getIdentifier()] = buckets;
  }

  return result;
}

// --- Stream with cost tracking ---

void KiloProvider::stream(const AgentHistory &history,
                          const ProviderOptions &opts,
                          std::function<void(const StreamEvent &)> onEvent) {
  // Ensure models and pricing are loaded BEFORE selecting account to avoid
  // holding accountsMutex_ while calling ensureModelsLoaded() (deadlock prevention).
  ensureModelsLoaded();

  std::string accountId;
  {
    std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
    auto accountOpt = getAvailableAccount(opts.modelId);
    if (!accountOpt) {
      onEvent(StreamError{"No Kilo account available (all exhausted or no anonymous for paid model)", 0, ""});
      return;
    }
    accountId = (*accountOpt)->getIdentifier();
  }

  auto wrappedOnEvent = [this, &opts, &onEvent, accountId](const StreamEvent &ev) {
    if (auto *m = std::get_if<AgentMetrics>(&ev)) {
      // Find account by identifier to avoid using a dangling pointer if
      // accounts_ vector reallocated.
      std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
      for (auto &acc : accounts_) {
        if (acc.getIdentifier() == accountId) {
          updateSpending(acc, opts.modelId, m->tokens.prompt, m->tokens.completion);
          break;
        }
      }
    }
    onEvent(ev);
  };

  BaseOpenAIProvider::stream(history, opts, wrappedOnEvent);
}

// Header customization for Kilo
std::map<std::string, std::string>
KiloProvider::buildHeadersForApiKey(const std::string &apiKey) {
  auto headers = BaseOpenAIProvider::buildHeadersForApiKey(apiKey);
  headers["User-Agent"] = "Firmius/1.0";
  headers["HTTP-Referer"] = "https://kilo.ai";
  headers["X-Title"] = "Firmius";
  headers["X-KILO-EDITORNAME"] = "Firmius";

  // Include organization ID if present on the account
  std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
  for (const auto &acc : accounts_) {
    if (acc.apiKey == apiKey) {
      auto orgIt = acc.metadata.find("kilo_organization_id");
      if (orgIt != acc.metadata.end() && !orgIt->second.empty()) {
        headers["X-KILO-ORGANIZATIONID"] = orgIt->second;
      }
      break;
    }
  }
  return headers;
}

} // namespace firmius::provider