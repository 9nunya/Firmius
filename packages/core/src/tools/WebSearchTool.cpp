#include "tools/WebSearchTool.hpp"
#include "environment/PermissionSuggestionEngine.hpp"
#include "harness/Harness.hpp"
#include "providers/LLMSearchProvider.hpp"
#include "providers/LLMSearchProviderRegistry.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::core {

using namespace firmius::shared;

shared::ToolMetadata WebSearchTool::getMetadata() const {
    return {"web_search", "Web operations: perform a web search and return results.", ToolScope::Web};
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
    // Network search gate.
    {
        PolicyRequest req;
        req.category = kCatNetworkSearch;
        req.query = input.query;
        req.toolName = "WebSearch";
        auto eval = Harness::instance().policyEngine().evaluate(req);
        if (eval.decision == PolicyDecision::Deny) {
            return shared::ToolResult::fail("Web search denied by policy.");
        }
        if (eval.decision == PolicyDecision::Ask) {
            shared::PermissionEscalationRequest esc;
            const auto &actx = ctx.agent.getContext();
            esc.threadId = actx.history ? actx.history->threadId : "";
            esc.agentId = actx.identity.id;
            esc.toolName = "WebSearch";
            esc.requestType = shared::PermissionRequestType::Read;
            esc.title = "Allow web search?";
            esc.message = "Approve search query: " + input.query;
            esc.severity = shared::CommandSeverity::LOW;
            esc.allowAlways = true;
            esc.category = req.category;
            esc.query = req.query;
            shared::CommandIntent dummy;
            auto suggestions =
                PermissionSuggestionEngine::generate(req, dummy);
            auto response =
                Harness::instance().requestPermissionEscalationWithSuggestions(
                    std::move(esc), std::move(suggestions));
            if (response == shared::PermissionResponse::Deny) {
                return shared::ToolResult::fail("Web search denied.");
            }
        }
    }

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

    // Token-waste pass 5: dropped `query` (echo of input) and `results[]`
    // (the formatted_text body already includes URLs+titles inline).
    // Keep `content` (the payload), `provider` (which provider answered),
    // and `queries_used` only when it differs from the input query.
    doc.AddMember("provider",
                  rapidjson::Value(provider.name().c_str(), a).Move(), a);

    if (!result.formatted_text.empty()) {
        doc.AddMember("content",
                      rapidjson::Value(result.formatted_text.c_str(), a).Move(),
                      a);
    } else {
        doc.AddMember("content", "", a);
    }

    bool emitQueriesUsed = false;
    for (const auto &q : result.queries_used) {
        if (q != input.query) { emitQueriesUsed = true; break; }
    }
    if (emitQueriesUsed) {
        rapidjson::Value queriesArr(rapidjson::kArrayType);
        for (const auto &q : result.queries_used) {
            queriesArr.PushBack(rapidjson::Value(q.c_str(), a).Move(), a);
        }
        doc.AddMember("queries_used", queriesArr, a);
    }

    return shared::ToolResult::ok(doc);
}

} // namespace firmius::core
