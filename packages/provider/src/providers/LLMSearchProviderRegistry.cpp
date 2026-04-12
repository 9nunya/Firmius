#include "providers/LLMSearchProviderRegistry.hpp"
#include <algorithm>

namespace firmius::provider {

LLMSearchProviderRegistry& LLMSearchProviderRegistry::instance() {
    static LLMSearchProviderRegistry registry;
    return registry;
}

void LLMSearchProviderRegistry::registerProvider(std::shared_ptr<LLMSearchProvider> provider) {
    if (!provider) return;
    std::lock_guard<std::mutex> lock(mutex_);
    providers_.push_back(std::move(provider));
}

void LLMSearchProviderRegistry::unregisterProvider(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_.erase(
        std::remove_if(providers_.begin(), providers_.end(),
                       [&name](const std::shared_ptr<LLMSearchProvider>& p) {
                           return p->name() == name;
                       }),
        providers_.end());
}

std::optional<std::reference_wrapper<LLMSearchProvider>> LLMSearchProviderRegistry::getFirstAvailable() const {
    std::vector<std::shared_ptr<LLMSearchProvider>> providers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        providers = providers_;
    }
    for (const auto& provider : providers) {
        if (provider->isAvailable()) {
            return std::ref(*provider);
        }
    }
    return std::nullopt;
}

std::vector<std::string> LLMSearchProviderRegistry::listProviderNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& provider : providers_) {
        names.push_back(provider->name());
    }
    return names;
}

} // namespace firmius::provider
