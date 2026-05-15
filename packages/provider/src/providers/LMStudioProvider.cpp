#include "providers/LMStudioProvider.hpp"
#include "EnvLoader.hpp"
#include <curl/curl.h>
#include <rapidjson/document.h>
#include <string>

namespace firmius::provider {

namespace {

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

}

LMStudioProvider::LMStudioProvider(const std::string& baseUrl)
    : BaseOpenAIProvider("lmstudio",
        baseUrl.empty()
            ? (shared::EnvLoader::get("LMSTUDIO_BASE_URL").empty()
                ? "http://localhost:1234"
                : shared::EnvLoader::get("LMSTUDIO_BASE_URL"))
            : baseUrl,
        "") {
    // LM Studio doesn't require API keys, so we don't add any accounts
}

bool LMStudioProvider::isConfigured() const {
    return true;
}

std::unique_ptr<APIKeyWizard> LMStudioProvider::beginConnectionWizard() {
    return nullptr;
}

std::map<std::string, std::string>
LMStudioProvider::buildHeadersForApiKey(const std::string& /*apiKey*/) {
    return {
        {"Content-Type", "application/json"}
    };
}

std::string LMStudioProvider::getChatUrl() const {
    return baseUrl + "/v1/chat/completions";
}

std::vector<firmius::shared::ModelInfo> LMStudioProvider::listModels() {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string url = baseUrl + "/api/v1/models";
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

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return {};
    }

    rapidjson::Document d;
    d.Parse(response.c_str());
    std::vector<firmius::shared::ModelInfo> models;

    if (d.IsObject() && d.HasMember("models") && d["models"].IsArray()) {
        for (const auto& m : d["models"].GetArray()) {
            // Skip non-llm models (e.g., embedding models)
            if (!m.HasMember("type") || !m["type"].IsString()) continue;
            std::string type = m["type"].GetString();
            if (type != "llm") continue;

            firmius::shared::ModelInfo mi;
            mi.id = m["key"].GetString();
            mi.provider = "lmstudio";



            // Context window
            if (m.HasMember("max_context_length") && m["max_context_length"].IsUint()) {
                mi.contextWindow = m["max_context_length"].GetUint();
            }

            // Modalities: always text, plus image if vision capable
            mi.modalities = {"text"};
            if (m.HasMember("capabilities") && m["capabilities"].IsObject()) {
                const auto& caps = m["capabilities"];
                if (caps.HasMember("vision") && caps["vision"].IsBool() && caps["vision"].GetBool()) {
                    mi.modalities.push_back("image");
                }
            }

            models.push_back(mi);
        }
    }

    return models;
}

}
