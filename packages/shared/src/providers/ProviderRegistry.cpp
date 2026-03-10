#include "providers/ProviderRegistry.hpp"

namespace firmius::provider {

ProviderRegistry& ProviderRegistry::instance() {
    static ProviderRegistry reg;
    return reg;
}

void ProviderRegistry::registerProvider(std::shared_ptr<IProvider> provider) {
    if (!provider) return;
    std::lock_guard<std::mutex> lock(mutex);
    providers[provider->getId()] = std::move(provider);
}

std::shared_ptr<IProvider> ProviderRegistry::getProvider(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = providers.find(id);
    if (it != providers.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> ProviderRegistry::listProviderIds() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> ids;
    for (const auto& [id, _] : providers) {
        ids.push_back(id);
    }
    return ids;
}

std::vector<std::shared_ptr<IProvider>> ProviderRegistry::listProviders() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::shared_ptr<IProvider>> list;
    list.reserve(providers.size());
    for (const auto& [_, provider] : providers) {
        list.push_back(provider);
    }
    return list;
}

}
