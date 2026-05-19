#ifndef FIRMIUS_CORE_WEBSEARCHTOOL_HPP
#define FIRMIUS_CORE_WEBSEARCHTOOL_HPP

#include "ITool.hpp"
#include "providers/LLMSearchProvider.hpp"
#include "providers/LLMSearchProviderRegistry.hpp"
#include <string>
#include <vector>

namespace firmius::core {

using firmius::shared::JSONSchema;
using firmius::shared::ToolContext;
using firmius::shared::ToolMetadata;
using firmius::shared::ToolResult;

/**
 * @brief Input parameters for the web_search tool.
 */
struct WebSearchInput {
    std::string query;                    ///< The search query string.
    std::vector<std::string> urls;        ///< Optional list of URLs to restrict search to.
};

/**
 * @brief Tool for performing web searches via registered LLMSearchProviders.
 */
class WebSearchTool : public shared::TypedTool<WebSearchInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;

    WebSearchInput transform(const rapidjson::Value &json) override;

    shared::ToolResult execute(const WebSearchInput& input, shared::ToolContext& ctx) override;
};

}

#endif
