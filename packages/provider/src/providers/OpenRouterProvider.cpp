#include "providers/OpenRouterProvider.hpp"
#include "EnvLoader.hpp"
#include <curl/curl.h>
#include <rapidjson/document.h>
#include <string>
#include <cstdlib>

namespace firmius::provider {

namespace {
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}
}

OpenRouterProvider::OpenRouterProvider(const std::string& apiKey)
    : BaseOpenAIProvider("openrouter", "https://openrouter.ai/api/v1", apiKey) {
    if (this->apiKey.empty()) {
        this->apiKey = shared::EnvLoader::get("OPENROUTER_API_KEY");
    }
}

std::map<std::string, std::string> OpenRouterProvider::getHeaders() {
    auto h = BaseOpenAIProvider::getHeaders();
    h["HTTP-Referer"] = "https://firmius.ai";
    h["X-Title"] = "Firmius";
    return h;
}

std::string OpenRouterProvider::getReasoningFieldName() const {
    return "reasoning";
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
