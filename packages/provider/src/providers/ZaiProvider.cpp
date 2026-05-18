#include "providers/ZaiProvider.hpp"
#include "EnvLoader.hpp"

namespace firmius::provider {

ZaiProvider::ZaiProvider(const std::string& apiKey)
    : BaseOpenAIProvider("zai", "https://api.z.ai/api/coding/paas/v4", apiKey) {
    // If no API key was provided and no accounts exist, try environment variable
    if (getAccountCount() == 0) {
        std::string key = shared::EnvLoader::get("ZAI_API_KEY");
        if (!key.empty()) {
            APIKeyAccount acc;
            acc.apiKey = key;
            acc.keyPrefix = extractKeyPrefix(key);
            acc.identifier = generateIdentifier();
            addAccount(acc);
        }
    }
}

std::vector<firmius::shared::ModelInfo> ZaiProvider::listModels() {
    auto models = BaseOpenAIProvider::listModels();
    for (auto& m : models) {
        m.provider = "zai";
        if (m.contextWindow == 0) m.contextWindow = 202000;
        // Token-caching pass: Z.ai's /v1/models endpoint does not publish
        // pricing, so BaseOpenAIProvider gives us 0 for both input and
        // cache. Per docs (https://docs.z.ai/guides/capabilities/cache)
        // cached tokens are billed at 50% of input price across all GLM
        // models. If a downstream catalog has populated input/output
        // pricing, derive the cache-read price as half of input. Cache
        // writes have no extra cost on Z.ai.
        if (m.pricePer1MCacheRead == 0.0 && m.pricePer1MInput > 0.0) {
            m.pricePer1MCacheRead = m.pricePer1MInput * 0.5;
        }
    }
    return models;
}

}
