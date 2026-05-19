#include "providers/GitlawbProvider.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <map>
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

GitlawbProvider::GitlawbProvider()
    : BaseOpenAIProvider("gitlawb",
                         "https://opengateway.gitlawb.com/v1/xiaomi-mimo",
                         "") {
    // Anonymous — no API key needed
}

bool GitlawbProvider::isConfigured() const {
    return true;
}

std::string GitlawbProvider::getReasoningFieldName() const {
    return "reasoning_content";
}

std::map<std::string, std::string>
GitlawbProvider::buildHeadersForApiKey(const std::string& /*apiKey*/) {
    return {{"Content-Type", "application/json"}};
}

std::vector<shared::ModelInfo> GitlawbProvider::listModels() {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string url = baseUrl + "/models";
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {};

    rapidjson::Document d;
    d.Parse(response.c_str());
    std::vector<shared::ModelInfo> models;

    if (d.IsObject() && d.HasMember("data") && d["data"].IsArray()) {
        for (const auto& m : d["data"].GetArray()) {
            if (!m.HasMember("id") || !m["id"].IsString()) continue;

            shared::ModelInfo mi;
            mi.id = m["id"].GetString();
            mi.provider = "gitlawb";

            // Context window — try common fields
            if (m.HasMember("context_length") && m["context_length"].IsUint()) {
                mi.contextWindow = m["context_length"].GetUint();
            } else if (m.HasMember("context_window") && m["context_window"].IsUint()) {
                mi.contextWindow = m["context_window"].GetUint();
            } else if (m.HasMember("max_context_length") && m["max_context_length"].IsUint()) {
                mi.contextWindow = m["max_context_length"].GetUint();
            }

            // Max output tokens
            if (m.HasMember("max_output_tokens") && m["max_output_tokens"].IsUint()) {
                mi.maxOutputTokens = m["max_output_tokens"].GetUint();
            }

            // Modalities
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

            // Reasoning support
            mi.supportsReasoning = false;
            if (m.HasMember("supported_features") && m["supported_features"].IsArray()) {
                for (const auto& feat : m["supported_features"].GetArray()) {
                    if (feat.IsString() && std::string(feat.GetString()) == "reasoning") {
                        mi.supportsReasoning = true;
                        break;
                    }
                }
            }
            // Also check capabilities object
            if (!mi.supportsReasoning && m.HasMember("capabilities") && m["capabilities"].IsObject()) {
                const auto& caps = m["capabilities"];
                if (caps.HasMember("reasoning") && caps["reasoning"].IsBool()) {
                    mi.supportsReasoning = caps["reasoning"].GetBool();
                }
            }

            models.push_back(mi);
        }
    }

    // Known-model metadata overrides. The Gitlawb /models endpoint returns
    // model IDs but does not populate context windows, output limits, or
    // capability flags reliably. We enrich each known id with the canonical
    // metadata from xiaomi-mimo's published model definitions; unknown ids
    // pass through as the API gave them.
    struct MimoSpec {
        std::uint32_t contextWindow;
        std::uint32_t maxOutputTokens;
        bool supportsReasoning;
        bool supportsVision;
    };
    static const std::map<std::string, MimoSpec> kMimoSpecs = {
        {"mimo-v2.5-pro", {1'000'000, 128'000, true, false}},
        {"mimo-v2-pro",   {1'000'000, 128'000, true, false}},
        {"mimo-v2.5",     {1'000'000, 128'000, true, true}},
        {"mimo-v2-omni",  {  256'000, 128'000, true, true}},
        {"mimo-v2-flash", {  256'000,  64'000, true, false}},
    };
    for (auto& mi : models) {
        auto it = kMimoSpecs.find(mi.id);
        if (it == kMimoSpecs.end()) continue;
        const auto& spec = it->second;
        if (mi.contextWindow == 0) mi.contextWindow = spec.contextWindow;
        if (mi.maxOutputTokens == 0) mi.maxOutputTokens = spec.maxOutputTokens;
        if (!mi.supportsReasoning && spec.supportsReasoning) {
            mi.supportsReasoning = true;
        }
        if (spec.supportsVision) {
            const bool hasImage = std::any_of(
                mi.modalities.begin(), mi.modalities.end(),
                [](const std::string& m) { return m == "image"; });
            if (!hasImage) mi.modalities.push_back("image");
        }
    }

    // If the API returned no models (or no /models endpoint), use hardcoded list
    if (models.empty()) {
        std::vector<shared::ModelVariant> effortVariants = {
            {"low", "{\"effort\":\"low\"}"},
            {"medium", "{\"effort\":\"medium\"}"},
            {"high", "{\"effort\":\"high\"}"},
            {"max", "{\"effort\":\"max\"}"},
        };

        auto addModel = [&](const std::string& id, uint32_t ctx, uint32_t maxOut,
                            bool reasoning, bool vision) {
            shared::ModelInfo mi;
            mi.id = id;
            mi.provider = "gitlawb";
            mi.contextWindow = ctx;
            mi.maxOutputTokens = maxOut;
            mi.supportsReasoning = reasoning;
            mi.modalities = {"text"};
            if (vision) mi.modalities.push_back("image");
            mi.variants = effortVariants;
            models.push_back(mi);
        };

        // mimo-v2.5-pro: reasoning, 1M ctx, 128K out, no vision
        addModel("mimo-v2.5-pro",   1000000, 128000, true,  false);
        // mimo-v2-pro: reasoning, 1M ctx, 128K out, no vision
        addModel("mimo-v2-pro",     1000000, 128000, true,  false);
        // mimo-v2.5: reasoning + vision, 1M ctx, 128K out
        addModel("mimo-v2.5",       1000000, 128000, true,  true);
        // mimo-v2-omni: reasoning + vision, 256K ctx, 128K out
        addModel("mimo-v2-omni",     256000, 128000, true,  true);
        // mimo-v2-flash: reasoning, 256K ctx, 64K out, no vision
        addModel("mimo-v2-flash",    256000,  64000, true,  false);
    }

    return models;
}

std::unique_ptr<APIKeyWizard> GitlawbProvider::beginConnectionWizard() {
    return nullptr; // Anonymous — no wizard needed
}

} // namespace firmius::provider
