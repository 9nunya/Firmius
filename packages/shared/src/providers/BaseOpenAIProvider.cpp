#include "providers/BaseOpenAIProvider.hpp"
#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <iostream>
#include <sstream>

namespace firmius::provider {

namespace {

// Improved write callback that handles SSE statefully
struct StreamContext {
    BaseOpenAIProvider* provider;
    std::function<void(const StreamEvent&)>* onEvent;
    std::string buffer;
};

size_t sseWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamContext*>(userdata);
    ctx->buffer.append(ptr, size * nmemb);
    
    size_t pos;
    while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);
        
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') line.pop_back();
        
        if (line.empty()) continue;
        
        ctx->provider->processSSELine(line, *(ctx->onEvent));
    }
    return size * nmemb;
}

std::string roleToString(Role r) {
    switch (r) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::ToolResult: return "tool";
    }
    return "user";
}
}

BaseOpenAIProvider::BaseOpenAIProvider(const std::string& baseUrl, const std::string& apiKey)
    : baseUrl(baseUrl), apiKey(apiKey) {}

void BaseOpenAIProvider::stream(const AgentHistory& history, const ProviderOptions& opts, 
                                std::function<void(const StreamEvent&)> onEvent) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("CURL init failed");

    std::string url = baseUrl + "/chat/completions";
    std::string body = prepareRequestBody(history, opts);
    
    struct curl_slist* headers = nullptr;
    auto headerMap = getHeaders();
    for (const auto& [k, v] : headerMap) {
        headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }

    StreamContext ctx{this, &onEvent, ""};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sseWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    
    if (res != CURLE_OK) {
        std::cerr << "CURL Error: " << curl_easy_strerror(res) << std::endl;
    } else if (response_code >= 400) {
        std::cerr << "API Error: " << response_code << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

void BaseOpenAIProvider::processSSELine(const std::string& line, std::function<void(const StreamEvent&)>& onEvent) {
    if (!line.starts_with("data: ")) return;
    std::string data = line.substr(6);
    if (data == "[DONE]") return;

    rapidjson::Document d;
    d.Parse(data.c_str());
    if (d.HasParseError() || !d.IsObject()) return;

    if (d.HasMember("choices") && d["choices"].IsArray() && d["choices"].Size() > 0) {
        const auto& choice = d["choices"][0];
        if (choice.HasMember("delta")) {
            const auto& delta = choice["delta"];
            
            if (delta.HasMember("content") && delta["content"].IsString()) {
                onEvent(TextChunk{ delta["content"].GetString() });
            }
            
            // Handle reasoning/thinking
            if (delta.HasMember("reasoning_content") && delta["reasoning_content"].IsString()) {
                onEvent(ThinkingChunk{ delta["reasoning_content"].GetString() });
            } else if (delta.HasMember("reasoning") && delta["reasoning"].IsString()) {
                onEvent(ThinkingChunk{ delta["reasoning"].GetString() });
            }

            if (delta.HasMember("tool_calls") && delta["tool_calls"].IsArray()) {
                for (const auto& tc : delta["tool_calls"].GetArray()) {
                    ToolCallChunk tcc;
                    if (tc.HasMember("id") && tc["id"].IsString()) tcc.id = tc["id"].GetString();
                    if (tc.HasMember("index")) tcc.index = tc["index"].GetUint();
                    if (tc.HasMember("function")) {
                        const auto& fn = tc["function"];
                        if (fn.HasMember("name") && fn["name"].IsString()) tcc.nameDelta = fn["name"].GetString();
                        if (fn.HasMember("arguments") && fn["arguments"].IsString()) tcc.argsDelta = fn["arguments"].GetString();
                    }
                    onEvent(tcc);
                }
            }
        }
    }
    
    if (d.HasMember("usage") && d["usage"].IsObject()) {
        const auto& usage = d["usage"];
        AgentMetrics am;
        am.tokens.prompt = usage.HasMember("prompt_tokens") ? usage["prompt_tokens"].GetUint() : 0;
        am.tokens.completion = usage.HasMember("completion_tokens") ? usage["completion_tokens"].GetUint() : 0;
        am.tokens.total = usage.HasMember("total_tokens") ? usage["total_tokens"].GetUint() : 0;
        if (usage.HasMember("completion_tokens_details") && usage["completion_tokens_details"].IsObject()) {
            const auto& details = usage["completion_tokens_details"];
            if (details.HasMember("reasoning_tokens")) am.tokens.reasoning = details["reasoning_tokens"].GetUint();
        }
        onEvent(am);
    }
}

std::vector<ModelInfo> BaseOpenAIProvider::listModels() {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string url = baseUrl + "/models";
    std::string response;
    
    auto writer = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
        return size * nmemb;
    };

    struct curl_slist* headers = nullptr;
    auto headerMap = getHeaders();
    for (const auto& [k, v] : headerMap) {
        headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<size_t(*)(char*,size_t,size_t,void*)>(writer));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    rapidjson::Document d;
    d.Parse(response.c_str());
    std::vector<ModelInfo> models;
    if (d.IsObject() && d.HasMember("data") && d["data"].IsArray()) {
        for (const auto& m : d["data"].GetArray()) {
            ModelInfo mi;
            mi.id = m["id"].GetString();
            mi.provider = "openai";
            if (m.HasMember("context_length")) mi.contextWindow = m["context_length"].GetUint();
            else if (m.HasMember("context_window")) mi.contextWindow = m["context_window"].GetUint();
            else if (m.HasMember("max_context_length")) mi.contextWindow = m["max_context_length"].GetUint();
            
            mi.modalities = {"text"};
            models.push_back(mi);
        }
    }
    return models;
}

std::map<std::string, std::string> BaseOpenAIProvider::getHeaders() {
    return {
        {"Authorization", "Bearer " + apiKey},
        {"Content-Type", "application/json"}
    };
}

std::string BaseOpenAIProvider::getReasoningFieldName() const {
    return "reasoning_content";
}

std::string BaseOpenAIProvider::prepareRequestBody(const AgentHistory& history, const ProviderOptions& opts) {
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();

    d.AddMember("model", rapidjson::Value(opts.modelId.c_str(), a), a);
    d.AddMember("temperature", opts.temperature, a);
    d.AddMember("stream", true, a);
    
    rapidjson::Value streamOpts(rapidjson::kObjectType);
    streamOpts.AddMember("include_usage", true, a);
    d.AddMember("stream_options", streamOpts, a);

    if (!opts.tools.empty()) {
        rapidjson::Value tools(rapidjson::kArrayType);
        for (const auto& t : opts.tools) {
            rapidjson::Value tool(rapidjson::kObjectType);
            tool.AddMember("type", "function", a);
            
            rapidjson::Value function(rapidjson::kObjectType);
            function.AddMember("name", rapidjson::Value(t.name.c_str(), a), a);
            function.AddMember("description", rapidjson::Value(t.description.c_str(), a), a);
            
            rapidjson::Document schemaDoc;
            schemaDoc.Parse(t.inputSchema.c_str());
            if (!schemaDoc.HasParseError()) {
                rapidjson::Value params;
                params.CopyFrom(schemaDoc, a);
                function.AddMember("parameters", params, a);
            }
            
            tool.AddMember("function", function, a);
            tools.PushBack(tool, a);
        }
        d.AddMember("tools", tools, a);
    }

    rapidjson::Value messages(rapidjson::kArrayType);
    for (const auto& turn : history.turns) {
        for (const auto& msg : turn.messages) {
            if (msg.role == Role::ToolResult) {
                // Tool result is a separate message with role: "tool" and tool_call_id
                for (const auto& part : msg.content) {
                    if (auto* res = std::get_if<ToolResultContent>(&part)) {
                        rapidjson::Value m(rapidjson::kObjectType);
                        m.AddMember("role", "tool", a);
                        m.AddMember("tool_call_id", rapidjson::Value(res->toolCallId.c_str(), a), a);
                        m.AddMember("content", rapidjson::Value(res->result.c_str(), a), a);
                        messages.PushBack(m, a);
                    }
                }
                continue;
            }

            rapidjson::Value m(rapidjson::kObjectType);
            m.AddMember("role", rapidjson::Value(roleToString(msg.role).c_str(), a), a);
            
            std::string textContent;
            std::string reasoningContent;
            rapidjson::Value toolCalls(rapidjson::kArrayType);

            for (const auto& part : msg.content) {
                if (auto* txt = std::get_if<TextContent>(&part)) {
                    textContent += txt->text;
                } else if (auto* thk = std::get_if<ThinkingContent>(&part)) {
                    reasoningContent += thk->thinking;
                } else if (auto* tcc = std::get_if<ToolCallContent>(&part)) {
                    rapidjson::Value tc(rapidjson::kObjectType);
                    tc.AddMember("id", rapidjson::Value(tcc->id.c_str(), a), a);
                    tc.AddMember("type", "function", a);
                    rapidjson::Value function(rapidjson::kObjectType);
                    function.AddMember("name", rapidjson::Value(tcc->name.c_str(), a), a);
                    function.AddMember("arguments", rapidjson::Value(tcc->args.c_str(), a), a);
                    tc.AddMember("function", function, a);
                    toolCalls.PushBack(tc, a);
                }
            }

            m.AddMember("content", rapidjson::Value(textContent.c_str(), a), a);
            if (!reasoningContent.empty()) {
                m.AddMember(rapidjson::Value(getReasoningFieldName().c_str(), a), rapidjson::Value(reasoningContent.c_str(), a), a);
            }
            if (toolCalls.Size() > 0) {
                m.AddMember("tool_calls", toolCalls, a);
            }
            messages.PushBack(m, a);
        }
    }
    d.AddMember("messages", messages, a);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    return buffer.GetString();
}

} // namespace firmius::provider
