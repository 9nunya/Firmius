#ifndef FIRMIUS_PROVIDER_GOOGLESEARCHPROVIDER_HPP
#define FIRMIUS_PROVIDER_GOOGLESEARCHPROVIDER_HPP

#include "providers/LLMSearchProvider.hpp"
#include <memory>
#include <string>

namespace firmius::provider {

// Forward declaration - we only need it as a weak reference
class AntigravityProvider;

/**
 * @brief Google search provider backed by Antigravity's grounding API.
 *
 * Uses the Antigravity OAuth/account infrastructure to authenticate.
 * search() calls the Antigravity API with googleSearch: {} in the tools array,
 * parses groundingMetadata from the response, and returns a SearchResult.
 * Auto-registers with LLMSearchProviderRegistry on construction.
 */
class GoogleSearchProvider : public LLMSearchProvider {
public:
    /**
     * @brief Constructs and auto-registers with LLMSearchProviderRegistry.
     * @param antigravity The AntigravityProvider to use for account/auth access.
     */
    explicit GoogleSearchProvider(AntigravityProvider* antigravity);
    ~GoogleSearchProvider() override = default;

    std::string name() const override;
    bool isAvailable() const override;
    SearchResult search(const std::string& query,
                        const std::vector<std::string>& urls = {}) override;

private:
    AntigravityProvider* antigravity_;  // non-owning reference
};

} // namespace firmius::provider

#endif
