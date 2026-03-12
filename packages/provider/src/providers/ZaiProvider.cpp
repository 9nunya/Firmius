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
    }
    return models;
}

}
