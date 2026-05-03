#ifndef FIRMIUS_PROVIDER_PROVIDER_REGISTRY_HPP
#define FIRMIUS_PROVIDER_PROVIDER_REGISTRY_HPP

#include "IProvider.hpp"
#include "ConfigLoader.hpp"
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <functional>
#include <set>

namespace firmius::provider {

/**
 * @brief Thread-safe registry for LLM providers.
 * Supports lazy instantiation via factory functions.
 */
class ProviderRegistry {
public:
    /**
     * @brief Factory function type for lazy provider creation.
     */
    using ProviderFactory = std::function<std::shared_ptr<IProvider>()>;

    /**
     * @brief Singleton instance access.
     */
    static ProviderRegistry& instance();

    /**
     * @brief Registers a provider instance directly (eager loading).
     * @param provider Shared pointer to the provider.
     */
    void registerProvider(std::shared_ptr<IProvider> provider);

    /**
     * @brief Registers a provider factory for lazy instantiation.
     * @param id Unique provider identifier.
     * @param factory Factory function to create the provider on-demand.
     */
    void registerProviderFactory(const std::string& id, ProviderFactory factory);

    /**
     * @brief Retrieves a provider by ID (lazy-loads if factory registered).
     * @param id The unique provider ID.
     * @return Shared pointer to the provider, or nullptr if not found.
     */
    std::shared_ptr<IProvider> getProvider(const std::string& id) const;

    /**
     * @brief Lists all registered provider IDs.
     */
    std::vector<std::string> listProviderIds() const;
    std::vector<std::shared_ptr<IProvider>> listProviders() const;
    void reloadConfigProviders(const std::map<std::string, shared::ProviderProfileConfig>& profiles);

private:
    ProviderRegistry() = default;
    ~ProviderRegistry() = default;
    ProviderRegistry(const ProviderRegistry&) = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;

    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<IProvider>> providers;
    std::map<std::string, ProviderFactory> factories;
    std::set<std::string> dynamicProviderIds;
};

}

#endif
