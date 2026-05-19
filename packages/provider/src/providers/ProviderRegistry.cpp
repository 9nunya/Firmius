#include "providers/ProviderRegistry.hpp"
#include "providers/ConfigurableAnthropicProvider.hpp"
#include "providers/ConfigurableOpenAIProvider.hpp"

namespace firmius::provider {

using namespace firmius::shared;

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
    builtinFactories[id] = factory;
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

std::vector<std::shared_ptr<IProvider>> ProviderRegistry::hydrateProviders() const {
    std::vector<std::pair<std::string, ProviderFactory>> pending_factories;
    std::vector<std::shared_ptr<IProvider>> loaded;
    {
        std::lock_guard<std::mutex> lock(mutex);
        loaded.reserve(providers.size() + factories.size());
        for (const auto &[_, provider] : providers) {
            if (provider) {
                loaded.push_back(provider);
            }
        }
        for (const auto &[id, factory] : factories) {
            if (!factory || providers.find(id) != providers.end()) {
                continue;
            }
            pending_factories.emplace_back(id, factory);
        }
    }

    for (const auto &[id, factory] : pending_factories) {
        auto provider = factory();
        if (!provider) {
            continue;
        }
        std::lock_guard<std::mutex> lock(mutex);
        auto& nonConstProviders =
            const_cast<std::map<std::string, std::shared_ptr<IProvider>>&>(providers);
        auto it = providers.find(id);
        if (it == providers.end()) {
            nonConstProviders[id] = provider;
            loaded.push_back(provider);
        } else if (it->second) {
            loaded.push_back(it->second);
        }
    }

    return loaded;
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

void ProviderRegistry::reloadConfigProviders(
    const std::map<std::string, shared::ProviderProfileConfig>& profiles) {
    std::lock_guard<std::mutex> lock(mutex);

    for (const auto& id : dynamicProviderIds) {
        providers.erase(id);
        auto builtinIt = builtinFactories.find(id);
        if (builtinIt != builtinFactories.end()) {
            factories[id] = builtinIt->second;
        } else {
            factories.erase(id);
        }
    }
    dynamicProviderIds.clear();

    for (const auto& [id, profile] : profiles) {
        providers.erase(id);
        if (!profile.enabled) {
            auto builtinIt = builtinFactories.find(id);
            if (builtinIt != builtinFactories.end()) {
                factories[id] = builtinIt->second;
            }
            continue;
        }
        dynamicProviderIds.insert(id);
        if (profile.kind == "anthropic") {
            factories[id] = [id, profile]() {
                return std::make_shared<ConfigurableAnthropicProvider>(id, profile);
            };
        } else {
            factories[id] = [id, profile]() {
                return std::make_shared<ConfigurableOpenAIProvider>(id, profile);
            };
        }
    }
}

}
