#ifndef FIRMIUS_PROVIDERS_LLMSEARCHPROVIDER_HPP
#define FIRMIUS_PROVIDERS_LLMSEARCHPROVIDER_HPP

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace firmius::provider {

/**
 * @brief Result returned by an LLMSearchProvider search operation.
 */
struct SearchResult {
    std::string formatted_text;
    std::vector<std::pair<std::string, std::string>> sources;  // url, title
    std::vector<std::string> queries_used;
    std::optional<std::string> error;
};

/**
 * @brief Abstract interface for LLM-backed web search providers.
 *
 * Implementations wrap external search APIs (e.g. Google via Antigravity)
 * and return structured, LLM-ready search results.
 */
class LLMSearchProvider {
public:
    virtual ~LLMSearchProvider() = default;

    /**
     * @brief Returns a human-readable name for this provider.
     */
    virtual std::string name() const = 0;

    /**
     * @brief Returns true if this provider is configured and ready to search.
     */
    virtual bool isAvailable() const = 0;

    /**
     * @brief Performs a web search.
     * @param query The search query string.
     * @param urls Optional list of URLs to restrict the search to.
     * @return SearchResult containing formatted text, sources, and metadata.
     */
    virtual SearchResult search(const std::string& query,
                                const std::vector<std::string>& urls = {}) = 0;
};

} // namespace firmius::provider

#endif
