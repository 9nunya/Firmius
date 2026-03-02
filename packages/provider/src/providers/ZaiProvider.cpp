#include "providers/ZaiProvider.hpp"
#include "EnvLoader.hpp"

namespace firmius::provider {

ZaiProvider::ZaiProvider(const std::string& apiKey)
    : BaseOpenAIProvider("zai", "https://api.z.ai/api/coding/paas/v4", apiKey) {
    if (this->apiKey.empty()) {
        this->apiKey = shared::EnvLoader::get("ZAI_API_KEY");
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
