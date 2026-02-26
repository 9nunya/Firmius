#include "providers/ZaiProvider.hpp"

namespace firmius::provider {

ZaiProvider::ZaiProvider(const std::string& apiKey)
    : BaseOpenAIProvider("https://api.z.ai/api/coding/paas/v4", apiKey) {}

std::vector<firmius::shared::ModelInfo> ZaiProvider::listModels() {
    auto models = BaseOpenAIProvider::listModels();
    for (auto& m : models) {
        m.provider = "zai";
        if (m.contextWindow == 0) m.contextWindow = 202000;
    }
    return models;
}

}
