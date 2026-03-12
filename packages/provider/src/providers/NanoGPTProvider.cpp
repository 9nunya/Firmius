#include "providers/NanoGPTProvider.hpp"
#include "EnvLoader.hpp"
#include <rapidjson/document.h>
#include <curl/curl.h>
#include <sstream>
#include <iostream>

namespace firmius::provider {

namespace {
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}
}

NanoGPTProvider::NanoGPTProvider(const std::vector<std::string>& initialKeys)
    : BaseOpenAIProvider("nanogpt", "https://nano-gpt.com/api/v1", "") {

    // Add initial keys if provided
    for (const auto& key : initialKeys) {
        if (!key.empty()) {
            APIKeyAccount acc;
            acc.apiKey = key;
            acc.keyPrefix = extractKeyPrefix(key);
            acc.identifier = generateIdentifier();
            addAccount(acc);
        }
    }

    // Load from environment if no keys provided
    if (getAccountCount() == 0) {
        for (int i = 1; i <= 10; ++i) {
            std::string key = shared::EnvLoader::get("NANOGPT_API_KEY_" + std::to_string(i));
            if (!key.empty()) {
                APIKeyAccount acc;
                acc.apiKey = key;
                acc.keyPrefix = extractKeyPrefix(key);
                acc.identifier = generateIdentifier();
                addAccount(acc);
            }
        }

        if (getAccountCount() == 0) {
            std::string primary = shared::EnvLoader::get("NANOGPT_API_KEY");
            if (!primary.empty()) {
                if (primary.find(',') != std::string::npos) {
                    std::stringstream ss(primary);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        if (!item.empty()) {
                            APIKeyAccount acc;
                            acc.apiKey = item;
                            acc.keyPrefix = extractKeyPrefix(item);
                            acc.identifier = generateIdentifier();
                            addAccount(acc);
                        }
                    }
                } else {
                    APIKeyAccount acc;
                    acc.apiKey = primary;
                    acc.keyPrefix = extractKeyPrefix(primary);
                    acc.identifier = generateIdentifier();
                    addAccount(acc);
                }
            }
        }
    }
}

std::map<std::string, std::string> NanoGPTProvider::getHeaders() {
    return BaseOpenAIProvider::getHeaders();
}

std::string NanoGPTProvider::getReasoningFieldName() const {
    return "reasoning_content";
}

std::vector<firmius::shared::ModelInfo> NanoGPTProvider::listModels() {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string url = baseUrl + "/models?detailed=true";
    std::string response;
    
    struct curl_slist* headers = nullptr;
    auto headerMap = getHeaders();
    for (const auto& [k, v] : headerMap) headers = curl_slist_append(headers, (k + ": " + v).c_str());

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
            mi.id = m["id"].GetString();
            mi.provider = "nanogpt";
            if (m.HasMember("context_length") && m["context_length"].IsUint()) mi.contextWindow = m["context_length"].GetUint();
            else if (m.HasMember("context_window") && m["context_window"].IsUint()) mi.contextWindow = m["context_window"].GetUint();
            mi.modalities = {"text"};
            if (m.HasMember("capabilities") && m["capabilities"].IsObject()) {
                const auto& caps = m["capabilities"];
                if (caps.HasMember("reasoning") && caps["reasoning"].GetBool()) mi.supportsReasoning = true;
                if (caps.HasMember("vision") && caps["vision"].GetBool()) mi.modalities.push_back("image");
            }
            // Dynamic pricing from NanoGPT API (USD per million tokens)
            if (m.HasMember("pricing") && m["pricing"].IsObject()) {
                const auto& pricing = m["pricing"];
                if (pricing.HasMember("prompt") && pricing["prompt"].IsNumber()) {
                    mi.pricePer1MInput = pricing["prompt"].GetDouble();
                }
                if (pricing.HasMember("completion") && pricing["completion"].IsNumber()) {
                    mi.pricePer1MOutput = pricing["completion"].GetDouble();
                }
            }
            models.push_back(mi);
        }
    }
    return models;
}

}
