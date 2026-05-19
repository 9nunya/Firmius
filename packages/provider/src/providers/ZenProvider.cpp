#include "providers/ZenProvider.hpp"
#include "EnvLoader.hpp"

namespace firmius::provider {

using namespace firmius::shared;

ZenProvider::ZenProvider(const std::string& apiKey)
    : BaseOpenAIProvider("zen", "https://opencode.ai/zen/v1", apiKey) {
    // If no API key was provided and no accounts exist, try environment variable
    if (getAccountCount() == 0) {
        for (int i = 1; i <= 10; ++i) {
            std::string key = shared::EnvLoader::get("ZEN_API_KEY_" + std::to_string(i));
            if (!key.empty()) {
                APIKeyAccount acc;
                acc.apiKey = key;
                acc.keyPrefix = extractKeyPrefix(key);
                acc.identifier = generateIdentifier();
                addAccount(acc);
                break;
            }
        }
        if (getAccountCount() == 0) {
            std::string key = shared::EnvLoader::get("ZEN_API_KEY");
            if (!key.empty()) {
                APIKeyAccount acc;
                acc.apiKey = key;
                acc.keyPrefix = extractKeyPrefix(key);
                acc.identifier = generateIdentifier();
                addAccount(acc);
            }
        }
    }
}

}
