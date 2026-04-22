#ifndef FIRMIUS_CORE_WEB_FETCH_TOOL_HPP
#define FIRMIUS_CORE_WEB_FETCH_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct WebFetchInput {
    std::string url;
};

class WebFetchTool : public shared::TypedTool<WebFetchInput> {
public:
    shared::ToolMetadata getMetadata() const override {
        return {"web_fetch", "Web operations: fetch content from a URL.", ToolScope::Web};
    }

    std::shared_ptr<shared::JSONSchema> getSchema() const override {
        return zObject({
            {"url", zString()->describe("The URL to fetch")}
        })->required({"url"});
    }

    START_MAPPING(WebFetchInput)
        MAP_STRING(url, "url")
    END_MAPPING

    shared::ToolResult execute(const WebFetchInput& input, shared::ToolContext& ctx) override;
};

}

#endif
