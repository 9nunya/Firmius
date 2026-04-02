#include "tools/WebSearchTool.hpp"
#include "providers/LLMSearchProvider.hpp"
#include "providers/LLMSearchProviderRegistry.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::core {

shared::ToolMetadata WebSearchTool::getMetadata() const {
    return {"web_search", "Perform a web search and return results", ToolScope::Web};
}

std::shared_ptr<shared::JSONSchema> WebSearchTool::getSchema() const {
    return zObject({
        {"query", zString()->describe("The search query string")},
        {"urls", zArray(zString())->describe("Optional list of URLs to restrict search to")}
    })->required({"query"});
}

WebSearchInput WebSearchTool::transform(const rapidjson::Value &json) {
    WebSearchInput input;
    (void)json;
    if (json.HasMember("query") && json["query"].IsString())
        input.query = json["query"].GetString();

    if (json.HasMember("urls") && json["urls"].IsArray()) {
        for (const auto &url : json["urls"].GetArray()) {
            if (url.IsString()) {
                input.urls.push_back(url.GetString());
            }
        }
    }

    return input;
}

shared::ToolResult WebSearchTool::execute(const WebSearchInput &input,
                                          shared::ToolContext &ctx) {
    if (!ctx.searchRegistry) {
        return shared::ToolResult::fail("No search providers configured");
    }

    auto providerOpt = ctx.searchRegistry->getFirstAvailable();
    if (!providerOpt.has_value()) {
        return shared::ToolResult::fail("All search providers unavailable");
    }

    auto &provider = providerOpt.value().get();
    auto result = provider.search(input.query, input.urls);

    if (result.error.has_value()) {
        return shared::ToolResult::fail(*result.error);
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();

    doc.AddMember("query", rapidjson::Value(input.query.c_str(), a).Move(), a);
    doc.AddMember("provider", rapidjson::Value(provider.name().c_str(), a).Move(), a);

    if (!result.formatted_text.empty()) {
        doc.AddMember("content", rapidjson::Value(result.formatted_text.c_str(), a).Move(), a);
    } else {
        doc.AddMember("content", "", a);
    }

    rapidjson::Value resultsArr(rapidjson::kArrayType);
    for (const auto &src : result.sources) {
        rapidjson::Value srcObj(rapidjson::kObjectType);
        srcObj.AddMember("url", rapidjson::Value(src.first.c_str(), a).Move(), a);
        srcObj.AddMember("title", rapidjson::Value(src.second.c_str(), a).Move(), a);
        resultsArr.PushBack(srcObj, a);
    }
    doc.AddMember("results", resultsArr, a);

    rapidjson::Value queriesArr(rapidjson::kArrayType);
    for (const auto &q : result.queries_used) {
        queriesArr.PushBack(rapidjson::Value(q.c_str(), a).Move(), a);
    }
    doc.AddMember("queries_used", queriesArr, a);

    return shared::ToolResult::ok(doc);
}

} // namespace firmius::core
