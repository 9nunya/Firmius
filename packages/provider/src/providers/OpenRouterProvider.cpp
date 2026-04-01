#include "providers/OpenRouterProvider.hpp"
#include "EnvLoader.hpp"
#include <curl/curl.h>
#include <rapidjson/document.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace firmius::provider {

namespace {
constexpr int kQuotaRefreshSeconds = 300;

std::string epochMillisToIso8601(int64_t epochMillis) {
    std::time_t seconds = static_cast<std::time_t>(epochMillis / 1000);
    std::tm tm = {};
#if defined(_WIN32)
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

std::optional<double> readOptionalNumber(const rapidjson::Value& obj,
                                         const char* key) {
    if (!obj.HasMember(key) || obj[key].IsNull()) {
        return std::nullopt;
    }
    if (obj[key].IsDouble()) {
        return obj[key].GetDouble();
    }
    if (obj[key].IsInt64()) {
        return static_cast<double>(obj[key].GetInt64());
    }
    if (obj[key].IsUint64()) {
        return static_cast<double>(obj[key].GetUint64());
    }
    if (obj[key].IsInt()) {
        return static_cast<double>(obj[key].GetInt());
    }
    if (obj[key].IsUint()) {
        return static_cast<double>(obj[key].GetUint());
    }
    return std::nullopt;
}

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<float> readQuotaFraction(const APIKeyAccount& acc) {
    auto it = acc.metadata.find("quota:openrouter");
    if (it == acc.metadata.end()) {
        return std::nullopt;
    }
    try {
        return std::clamp(std::stof(it->second), 0.0f, 1.0f);
    } catch (...) {
        return std::nullopt;
    }
}
}

OpenRouterProvider::OpenRouterProvider(const std::string& apiKey)
    : BaseOpenAIProvider("openrouter", "https://openrouter.ai/api/v1", apiKey) {
    // If no API key was provided and no accounts exist, try environment variable
    if (getAccountCount() == 0) {
        std::string envKey = shared::EnvLoader::get("OPENROUTER_API_KEY");
        if (!envKey.empty()) {
            APIKeyAccount acc;
            acc.apiKey = envKey;
            acc.keyPrefix = extractKeyPrefix(envKey);
            acc.identifier = generateIdentifier();
            addAccount(acc);
        }
    }
}

std::optional<OpenRouterProvider::KeyQuotaInfo>
OpenRouterProvider::parseKeyQuotaInfoResponse(const std::string& response) {
    rapidjson::Document d;
    d.Parse(response.c_str());
    if (!d.IsObject() || !d.HasMember("data") || !d["data"].IsObject()) {
        return std::nullopt;
    }

    const auto& data = d["data"];
    KeyQuotaInfo info;
    info.limit = readOptionalNumber(data, "limit");
    info.limitRemaining = readOptionalNumber(data, "limit_remaining");

    if (data.HasMember("limit_reset") && data["limit_reset"].IsString()) {
        info.limitReset = data["limit_reset"].GetString();
    }
    if (auto usage = readOptionalNumber(data, "usage")) {
        info.usage = *usage;
    }
    if (data.HasMember("label") && data["label"].IsString()) {
        info.label = data["label"].GetString();
    }
    if (data.HasMember("is_free_tier") && data["is_free_tier"].IsBool()) {
        info.isFreeTier = data["is_free_tier"].GetBool();
    }

    return info;
}

std::map<std::string, std::string>
OpenRouterProvider::buildHeadersForApiKey(const std::string& apiKey) {
    auto h = BaseOpenAIProvider::buildHeadersForApiKey(apiKey);
    h["HTTP-Referer"] = "https://firmius.ai";
    h["X-Title"] = "Firmius";
    return h;
}

std::string OpenRouterProvider::getReasoningFieldName() const {
    return "reasoning";
}

void OpenRouterProvider::refreshQuotas() {
    std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
    bool needsSave = false;
    const int64_t now = nowSeconds();

    for (auto& acc : accounts_) {
        int64_t lastRefresh = 0;
        auto it = acc.metadata.find("quota_refresh_ts:openrouter");
        if (it != acc.metadata.end()) {
            try {
                lastRefresh = std::stoll(it->second);
            } catch (...) {
                lastRefresh = 0;
            }
        }
        if (lastRefresh > 0 && now - lastRefresh < kQuotaRefreshSeconds) {
            continue;
        }

        auto info = fetchKeyQuotaInfo(acc);
        if (!info.has_value()) {
            continue;
        }

        if (info->limit.has_value() && info->limitRemaining.has_value() &&
            *info->limit > 0.0) {
            const float fraction = static_cast<float>(
                std::clamp(*info->limitRemaining / *info->limit, 0.0, 1.0));
            acc.metadata["quota:openrouter"] = std::to_string(fraction);
            acc.metadata["quota_limit:openrouter"] = std::to_string(*info->limit);
            acc.metadata["quota_remaining:openrouter"] =
                std::to_string(*info->limitRemaining);
            acc.metadata["quota_note:openrouter"] = "$" +
                std::to_string(*info->limitRemaining) + " remaining";
            acc.metadata.erase("quota_exhausted_reset:openrouter");
        } else if (info->isFreeTier) {
            auto resetIt = acc.metadata.find("quota_exhausted_reset:openrouter");
            if (resetIt != acc.metadata.end()) {
                try {
                    int64_t resetTs = std::stoll(resetIt->second);
                    if (now < resetTs) {
                        acc.metadata["quota:openrouter"] = "0";
                        acc.metadata["quota_note:openrouter"] =
                            "Free tier exhausted — resets at " + resetIt->second;
                    } else {
                        acc.metadata.erase("quota_exhausted_reset:openrouter");
                        acc.metadata["quota:openrouter"] = "1";
                        acc.metadata["quota_note:openrouter"] =
                            "Free tier key (daily quota available)";
                    }
                } catch (...) {
                    acc.metadata.erase("quota_exhausted_reset:openrouter");
                    acc.metadata["quota:openrouter"] = "1";
                    acc.metadata["quota_note:openrouter"] =
                        "Free tier key (daily quota available)";
                }
            } else {
                acc.metadata["quota:openrouter"] = "1";
                acc.metadata["quota_note:openrouter"] =
                    "Free tier key (daily quota available)";
            }
            acc.metadata.erase("quota_limit:openrouter");
            acc.metadata.erase("quota_remaining:openrouter");
        } else {
            acc.metadata["quota:openrouter"] = "1";
            acc.metadata.erase("quota_limit:openrouter");
            acc.metadata.erase("quota_remaining:openrouter");
            acc.metadata["quota_note:openrouter"] = "No hard spend cap";
        }

        acc.metadata["quota_usage:openrouter"] = std::to_string(info->usage);
        acc.metadata["quota_is_free_tier:openrouter"] =
            info->isFreeTier ? "true" : "false";
        acc.metadata["quota_refresh_ts:openrouter"] = std::to_string(now);
        if (!info->limitReset.empty()) {
            acc.metadata["quota_reset:openrouter"] = info->limitReset;
        } else {
            acc.metadata.erase("quota_reset:openrouter");
        }
        if (!info->label.empty()) {
            acc.metadata["quota_label:openrouter"] = info->label;
        }
        needsSave = true;
    }

    if (needsSave) {
        saveAccounts();
    }
}

std::map<std::string, std::vector<firmius::shared::QuotaBucket>>
OpenRouterProvider::getAllQuotas() const {
    std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
    std::map<std::string, std::vector<firmius::shared::QuotaBucket>> result;

    for (const auto& acc : accounts_) {
        std::vector<firmius::shared::QuotaBucket> buckets;
        const float remaining = readQuotaFraction(acc).value_or(1.0f);

        std::string reset;
        auto resetIt = acc.metadata.find("quota_exhausted_reset:openrouter");
        if (resetIt != acc.metadata.end() && !resetIt->second.empty()) {
            reset = resetIt->second;
        } else {
            auto legacyResetIt = acc.metadata.find("quota_reset:openrouter");
            if (legacyResetIt != acc.metadata.end() && !legacyResetIt->second.empty()) {
                reset = legacyResetIt->second;
            }
        }

        if (!reset.empty()) {
            bool isAllDigits = !reset.empty() &&
                std::all_of(reset.begin(), reset.end(), [](unsigned char c) { return std::isdigit(c); });
            if (isAllDigits) {
                try {
                    int64_t val = std::stoll(reset);
                    if (val > 10000000000LL) {
                        reset = epochMillisToIso8601(val);
                    } else {
                        reset = epochMillisToIso8601(val * 1000);
                    }
                } catch (...) {
                }
            }
        }

        std::string note;
        auto noteIt = acc.metadata.find("quota_note:openrouter");
        if (noteIt != acc.metadata.end()) {
            note = noteIt->second;
        }

        buckets.push_back(firmius::shared::QuotaBucket{
            "openrouter", remaining, reset, note});
        result[acc.getIdentifier()] = buckets;
    }

    return result;
}

std::optional<APIKeyAccount *>
OpenRouterProvider::getAvailableAccount(
    const std::optional<std::string> &modelId) {
    std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
    if (accounts_.empty()) {
        return std::nullopt;
    }

    const int64_t now = getNowSeconds();

    int startIdx = (lastUsedIndex_.load(std::memory_order_relaxed) >= 0 &&
                    lastUsedIndex_.load(std::memory_order_relaxed) < static_cast<int>(accounts_.size()))
                   ? lastUsedIndex_.load(std::memory_order_relaxed)
                   : 0;

    int bestIdx = -1;
    float bestQuota = -1.0f;
    for (int i = 0; i < static_cast<int>(accounts_.size()); ++i) {
        int idx = (startIdx + i) % static_cast<int>(accounts_.size());
        auto& acc = accounts_[idx];
        if (acc.rateLimited && now > acc.backoffUntil) {
            acc.rateLimited = false;
        }
        if (acc.rateLimited) {
            continue;
        }

        const float quota = readQuotaFraction(acc).value_or(1.0f);
        if (bestIdx < 0 || quota > bestQuota) {
            bestIdx = idx;
            bestQuota = quota;
        }
    }

    if (bestIdx >= 0) {
        lastUsedIndex_.store(bestIdx, std::memory_order_relaxed);
        return &accounts_[bestIdx];
    }

    return BaseOpenAIProvider::getAvailableAccount(modelId);
}

BaseOpenAIProvider::RateLimitSwitchResult
OpenRouterProvider::handleRateLimitAndMaybeSwitch(
    APIKeyAccount& currentAccount,
    const std::optional<std::string>& modelId,
    int headerDelayMs,
    int rateLimitAttempt,
    int64_t rateLimitResetMs) {

    auto result = BaseOpenAIProvider::handleRateLimitAndMaybeSwitch(
        currentAccount, modelId, headerDelayMs, rateLimitAttempt);

    if (rateLimitResetMs > 0) {
        std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
        currentAccount.metadata["quota:openrouter"] = "0";
        currentAccount.metadata["quota_exhausted_reset:openrouter"] =
            epochMillisToIso8601(rateLimitResetMs);
        currentAccount.metadata["quota_note:openrouter"] =
            "Free tier exhausted";
        saveAccounts();
    }

    return result;
}

std::optional<OpenRouterProvider::KeyQuotaInfo>
OpenRouterProvider::fetchKeyQuotaInfo(const APIKeyAccount& acc) const {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return std::nullopt;
    }

    std::string url = baseUrl + "/key";
    std::string response;
    struct curl_slist* headers = nullptr;
    auto headerMap =
        const_cast<OpenRouterProvider *>(this)->buildHeadersForApiKey(acc.apiKey);
    for (const auto& [k, v] : headerMap) {
        headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    const CURLcode res = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpStatus < 200 || httpStatus >= 300) {
        return std::nullopt;
    }

    return parseKeyQuotaInfoResponse(response);
}

std::vector<firmius::shared::ModelInfo> OpenRouterProvider::listModels() {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string url = baseUrl + "/models";
    std::string response;

    struct curl_slist* headers = nullptr;
    auto headerMap = getHeaders();
    for (const auto& [k, v] : headerMap) {
        headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    rapidjson::Document d;
    d.Parse(response.c_str());
    std::vector<firmius::shared::ModelInfo> models;

    if (d.IsObject() && d.HasMember("data") && d["data"].IsArray()) {
        for (const auto& m : d["data"].GetArray()) {
            firmius::shared::ModelInfo mi;
            if (!m.HasMember("id") || !m["id"].IsString()) continue;
            mi.id = m["id"].GetString();
            mi.provider = "openrouter";

            if (m.HasMember("context_length") && m["context_length"].IsUint()) {
                mi.contextWindow = m["context_length"].GetUint();
            }

            // Modalities
            mi.modalities = {"text"};
            if (m.HasMember("architecture") && m["architecture"].IsObject()) {
                const auto& arch = m["architecture"];
                if (arch.HasMember("input_modalities") && arch["input_modalities"].IsArray()) {
                    for (const auto& mod : arch["input_modalities"].GetArray()) {
                        if (mod.IsString()) {
                            std::string modStr = mod.GetString();
                            if (modStr == "image" || modStr == "audio") {
                                mi.modalities.push_back(modStr);
                            }
                        }
                    }
                }
            }

            // Dynamic pricing from OpenRouter (STRING values, USD per single token)
            if (m.HasMember("pricing") && m["pricing"].IsObject()) {
                const auto& pricing = m["pricing"];
                auto parsePrice = [](const rapidjson::Value& obj, const char* key) -> double {
                    if (obj.HasMember(key) && obj[key].IsString()) {
                        return std::strtod(obj[key].GetString(), nullptr) * 1'000'000.0;
                    }
                    return 0.0;
                };
                mi.pricePer1MInput = parsePrice(pricing, "prompt");
                mi.pricePer1MOutput = parsePrice(pricing, "completion");
                mi.pricePer1MCacheRead = parsePrice(pricing, "input_cache_read");
                mi.pricePer1MCacheWrite = parsePrice(pricing, "input_cache_write");
            }

            models.push_back(mi);
        }
    }
    return models;
}

}
