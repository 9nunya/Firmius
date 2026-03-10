#ifndef FIRMIUS_PROVIDER_PROVIDER_REGISTRY_HPP
#define FIRMIUS_PROVIDER_PROVIDER_REGISTRY_HPP

#include "IProvider.hpp"
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace firmius::provider {

/**
 * @brief Thread-safe registry for LLM providers.
 * Manages shared instances of providers identified by unique IDs.
 */
class ProviderRegistry {
public:
    /**
     * @brief Singleton instance access.
     */
    static ProviderRegistry& instance();

    /**
     * @brief Registers a provider instance.
     * @param provider Shared pointer to the provider.
     */
    void registerProvider(std::shared_ptr<IProvider> provider);

    /**
     * @brief Retrieves a provider by ID.
     * @param id The unique provider ID.
     * @return Shared pointer to the provider, or nullptr if not found.
     */
    std::shared_ptr<IProvider> getProvider(const std::string& id) const;

    /**
     * @brief Lists all registered provider IDs.
     */
    std::vector<std::string> listProviderIds() const;
    std::vector<std::shared_ptr<IProvider>> listProviders() const;

private:
    ProviderRegistry() = default;
    ~ProviderRegistry() = default;
    ProviderRegistry(const ProviderRegistry&) = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;

    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<IProvider>> providers;
};

}

#endif
