#ifndef FIRMIUS_CORE_WEBFETCHTOOL_HPP
#define FIRMIUS_CORE_WEBFETCHTOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

using firmius::shared::JSONSchema;
using firmius::shared::ToolContext;
using firmius::shared::ToolMetadata;
using firmius::shared::ToolResult;
using firmius::shared::ToolScope;
using firmius::shared::zObject;
using firmius::shared::zString;

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
