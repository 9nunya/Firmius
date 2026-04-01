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

void ProviderRegistry::registerProviderFactory(const std::string& id, ProviderFactory factory) {
    if (!factory) return;
    std::lock_guard<std::mutex> lock(mutex);
    factories[id] = std::move(factory);
}

std::shared_ptr<IProvider> ProviderRegistry::getProvider(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex);
    
    // First check if already instantiated
    auto it = providers.find(id);
    if (it != providers.end()) {
        return it->second;
    }
    
    // Check if we have a factory for lazy instantiation
    auto factory_it = factories.find(id);
    if (factory_it != factories.end()) {
        auto provider = factory_it->second();
        if (provider) {
            // Use non-const access through const_cast (safe because we hold the lock)
            auto& nonConstProviders = const_cast<std::map<std::string, std::shared_ptr<IProvider>>&>(providers);
            nonConstProviders[id] = provider;
            return provider;
        }
    }
    
    return nullptr;
}

std::vector<std::string> ProviderRegistry::listProviderIds() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> ids;
    
    // Add instantiated providers
    for (const auto& [id, _] : providers) {
        ids.push_back(id);
    }
    
    // Add factory-registered providers (not yet instantiated)
    for (const auto& [id, _] : factories) {
        if (providers.find(id) == providers.end()) {
            ids.push_back(id);
        }
    }
    
    return ids;
}

std::vector<std::shared_ptr<IProvider>> ProviderRegistry::listProviders() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::shared_ptr<IProvider>> list;
    
    // Return only instantiated providers
    for (const auto& [_, provider] : providers) {
        list.push_back(provider);
    }
    
    return list;
}

}
