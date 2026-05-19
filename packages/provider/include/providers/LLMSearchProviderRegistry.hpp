#ifndef FIRMIUS_PROVIDER_LLMSEARCHPROVIDERREGISTRY_HPP
#define FIRMIUS_PROVIDER_LLMSEARCHPROVIDERREGISTRY_HPP

#include "providers/LLMSearchProvider.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace firmius::provider {

/**
 * @brief Thread-safe registry for LLMSearchProvider instances.
 *
 * Holds a collection of search providers and returns the first available one.
 */
class LLMSearchProviderRegistry {
public:
    /**
     * @brief Singleton instance access.
     */
    static LLMSearchProviderRegistry& instance();

    /**
     * @brief Registers a search provider.
     * @param provider Shared pointer to the provider.
     */
    void registerProvider(std::shared_ptr<LLMSearchProvider> provider);

    /**
     * @brief Unregisters a provider by name.
     * @param name The provider name to remove.
     */
    void unregisterProvider(const std::string& name);

    /**
     * @brief Returns the first available (configured and ready) provider.
     * @return reference_wrapper to the provider, or std::nullopt if none available.
     */
    std::optional<std::reference_wrapper<LLMSearchProvider>> getFirstAvailable() const;

    /**
     * @brief Lists all registered provider names.
     */
    std::vector<std::string> listProviderNames() const;

private:
    LLMSearchProviderRegistry() = default;
    ~LLMSearchProviderRegistry() = default;
    LLMSearchProviderRegistry(const LLMSearchProviderRegistry&) = delete;
    LLMSearchProviderRegistry& operator=(const LLMSearchProviderRegistry&) = delete;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<LLMSearchProvider>> providers_;
};

} // namespace firmius::provider

#endif
