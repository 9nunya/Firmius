#include "providers/ChutesProvider.hpp"
#include "EnvLoader.hpp"
#include <curl/curl.h>
#include <rapidjson/document.h>
#include <string>

namespace firmius::provider {

using namespace firmius::shared;

namespace {
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}
}

ChutesProvider::ChutesProvider(const std::string& apiKey)
    : BaseOpenAIProvider("chutes", "https://llm.chutes.ai/v1", apiKey) {
    // If no API key was provided and no accounts exist, try environment variable
    if (getAccountCount() == 0) {
        std::string key = shared::EnvLoader::get("CHUTES_API_KEY");
        if (!key.empty()) {
            APIKeyAccount acc;
            acc.apiKey = key;
            acc.keyPrefix = extractKeyPrefix(key);
            acc.identifier = generateIdentifier();
            addAccount(acc);
        }
    }
}

std::string ChutesProvider::getReasoningFieldName() const {
    return "reasoning_content";
}

std::vector<firmius::shared::ModelInfo> ChutesProvider::listModels() {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    // Chutes /v1/models is public (no auth required for listing)
    std::string url = baseUrl + "/models";
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    // Auth is optional for model listing but include if available
    auto account = getAvailableAccount();
    if (account && !(*account)->apiKey.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + (*account)->apiKey).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

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
            mi.provider = "chutes";

            if (m.HasMember("context_length") && m["context_length"].IsUint()) {
                mi.contextWindow = m["context_length"].GetUint();
            } else if (m.HasMember("max_model_len") && m["max_model_len"].IsUint()) {
                mi.contextWindow = m["max_model_len"].GetUint();
            }

            // Modalities from input_modalities array
            mi.modalities = {"text"};
            if (m.HasMember("input_modalities") && m["input_modalities"].IsArray()) {
                for (const auto& mod : m["input_modalities"].GetArray()) {
                    if (mod.IsString()) {
                        std::string modStr = mod.GetString();
                        if (modStr == "image" || modStr == "audio") {
                            mi.modalities.push_back(modStr);
                        }
                    }
                }
            }

            // Reasoning support from supported_features
            if (m.HasMember("supported_features") && m["supported_features"].IsArray()) {
                for (const auto& feat : m["supported_features"].GetArray()) {
                    if (feat.IsString() && std::string(feat.GetString()) == "reasoning") {
                        mi.supportsReasoning = true;
                        break;
                    }
                }
            }

            // Dynamic pricing from Chutes API (USD per million tokens, numbers)
            if (m.HasMember("pricing") && m["pricing"].IsObject()) {
                const auto& pricing = m["pricing"];
                if (pricing.HasMember("prompt") && pricing["prompt"].IsNumber()) {
                    mi.pricePer1MInput = pricing["prompt"].GetDouble();
                }
                if (pricing.HasMember("completion") && pricing["completion"].IsNumber()) {
                    mi.pricePer1MOutput = pricing["completion"].GetDouble();
                }
                if (pricing.HasMember("input_cache_read") && pricing["input_cache_read"].IsNumber()) {
                    mi.pricePer1MCacheRead = pricing["input_cache_read"].GetDouble();
                }
            }

            models.push_back(mi);
        }
    }
    return models;
}

}
