#include "providers/ZenProvider.hpp"
#include "EnvLoader.hpp"

namespace firmius::provider {

ZenProvider::ZenProvider(const std::string& apiKey)
    : BaseOpenAIProvider("zen", "https://opencode.ai/zen/v1", apiKey) {
    if (this->apiKey.empty()) {
        for (int i = 1; i <= 10; ++i) {
            std::string key = shared::EnvLoader::get("ZEN_API_KEY_" + std::to_string(i));
            if (!key.empty()) {
                this->apiKey = key;
                break;
            }
        }
        if (this->apiKey.empty()) {
            this->apiKey = shared::EnvLoader::get("ZEN_API_KEY");
        }
    }
}

}
