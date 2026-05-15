#include "providers/LLMSearchProviderRegistry.hpp"
#include "providers/ProviderRegistry.hpp"
#include <algorithm>

namespace firmius::provider {

namespace {

bool hasProviderNamed(
    const std::vector<std::shared_ptr<LLMSearchProvider>> &providers,
    const std::string &name) {
    return std::any_of(
        providers.begin(), providers.end(),
        [&name](const std::shared_ptr<LLMSearchProvider> &provider) {
            return provider && provider->name() == name;
        });
}

std::optional<std::reference_wrapper<LLMSearchProvider>>
findFirstAvailableProvider(
    const std::vector<std::shared_ptr<LLMSearchProvider>> &providers) {
    for (const auto &provider : providers) {
        if (provider && provider->isAvailable()) {
            return std::ref(*provider);
        }
    }
    return std::nullopt;
}

} // namespace

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

    if (auto available = findFirstAvailableProvider(providers)) {
        return available;
    }

    // Antigravity owns the built-in Google-backed search provider, but that
    // provider is only registered when Antigravity is instantiated. Web search
    // should not depend on the active chat model having already done that.
    if (!hasProviderNamed(providers, "google-search")) {
        ProviderRegistry::instance().getProvider("antigravity");

        {
            std::lock_guard<std::mutex> lock(mutex_);
            providers = providers_;
        }

        if (auto available = findFirstAvailableProvider(providers)) {
            return available;
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
