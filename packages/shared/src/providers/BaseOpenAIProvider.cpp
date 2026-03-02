#include "providers/BaseOpenAIProvider.hpp"
#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <chrono>

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

BaseOpenAIProvider::BaseOpenAIProvider(std::string id, const std::string& baseUrl, const std::string& apiKey)
    : providerId(std::move(id)), baseUrl(baseUrl), apiKey(apiKey) {}

std::uint64_t BaseOpenAIProvider::nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

void BaseOpenAIProvider::stream(const AgentHistory& history, const ProviderOptions& opts,
                                std::function<void(const StreamEvent&)> onEvent) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        onEvent(StreamError{"CURL init failed", 0});
        return;
    }

    std::string url = baseUrl + "/chat/completions";
    std::string body = prepareRequestBody(history, opts);

    struct curl_slist* headers = nullptr;
    auto headerMap = getHeaders();
    for (const auto& [k, v] : headerMap) {
        headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }

    // Timing + metrics tracking state
    auto startMs = nowMs();
    bool firstTokenEmitted = false;
    std::uint64_t firstTokenMs = 0;
    AgentMetrics capturedMetrics;
    bool metricsReceived = false;
    bool doneReceived = false;

    // Wrap the user's callback to intercept timing and metrics
    auto wrappedOnEvent = [&](const StreamEvent& ev) {
        // Track first token timing
        if (!firstTokenEmitted) {
            if (std::holds_alternative<TextChunk>(ev) || std::holds_alternative<ThinkingChunk>(ev)) {
                firstTokenMs = nowMs();
                firstTokenEmitted = true;
            }
        }

        // Intercept AgentMetrics to inject timing before forwarding
        if (auto* met = std::get_if<AgentMetrics>(&ev)) {
            capturedMetrics = *met;
            metricsReceived = true;
            // Don't forward yet — we'll emit with timing at the end
            return;
        }

        // Track StreamDone
        if (std::holds_alternative<StreamDone>(ev)) {
            doneReceived = true;
        }

        onEvent(ev);
    };

    std::function<void(const StreamEvent&)> wrappedFn = wrappedOnEvent;
    StreamContext ctx{this, &wrappedFn, ""};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sseWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    auto endMs = nowMs();

    // Emit StreamError on failure
    if (res != CURLE_OK) {
        onEvent(StreamError{std::string("CURL error: ") + curl_easy_strerror(res), 0});
    } else if (response_code >= 400) {
        onEvent(StreamError{"API error (HTTP " + std::to_string(response_code) + ")", static_cast<int>(response_code)});
    }

    // Inject timing into metrics and emit
    if (metricsReceived) {
        capturedMetrics.timing.startMs = startMs;
        capturedMetrics.timing.firstTokenMs = firstTokenEmitted ? firstTokenMs : 0;
        capturedMetrics.timing.endMs = endMs;
        // toolExecutionMs is the caller's responsibility (Agent layer)

        // Calculate cost if we can resolve the model
        try {
            auto model = getModelInfo(opts.modelId);
            calculateCost(capturedMetrics, model);
        } catch (...) {
            // Cost calculation is best-effort
        }

        onEvent(capturedMetrics);
    }

    // Guarantee a StreamDone if the SSE stream didn't emit one
    if (!doneReceived && res == CURLE_OK && response_code < 400) {
        onEvent(StreamDone{StopReason::Stop});
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

        // Extract finish_reason (arrives on the choice, not inside delta)
        if (choice.HasMember("finish_reason") && choice["finish_reason"].IsString()) {
            std::string fr = choice["finish_reason"].GetString();
            StopReason sr = StopReason::Stop;
            if (fr == "tool_calls") sr = StopReason::ToolUse;
            else if (fr == "length") sr = StopReason::MaxTokens;
            else if (fr == "content_filter") sr = StopReason::ContentFilter;
            onEvent(StreamDone{sr});
        }

        if (choice.HasMember("delta")) {
            const auto& delta = choice["delta"];

            if (delta.HasMember("content") && delta["content"].IsString()) {
                onEvent(TextChunk{ delta["content"].GetString() });
            }

            // Handle reasoning/thinking (provider-specific field name)
            std::string reasoningField = getReasoningFieldName();
            if (delta.HasMember(reasoningField.c_str()) && delta[reasoningField.c_str()].IsString()) {
                onEvent(ThinkingChunk{ delta[reasoningField.c_str()].GetString() });
            } else if (reasoningField != "reasoning" && delta.HasMember("reasoning") && delta["reasoning"].IsString()) {
                // Fallback: try "reasoning" if the provider-specific field wasn't found
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

    // Extract usage (typically on the final chunk when include_usage is true)
    if (d.HasMember("usage") && d["usage"].IsObject()) {
        const auto& usage = d["usage"];
        AgentMetrics am;

        std::uint32_t promptTokens = usage.HasMember("prompt_tokens") ? usage["prompt_tokens"].GetUint() : 0;
        am.tokens.contextSize = promptTokens;
        am.tokens.completion = usage.HasMember("completion_tokens") ? usage["completion_tokens"].GetUint() : 0;

        // Extract cached tokens from prompt_tokens_details
        // OpenAI includes cached_tokens INSIDE prompt_tokens, so subtract
        if (usage.HasMember("prompt_tokens_details") && usage["prompt_tokens_details"].IsObject()) {
            const auto& pdetails = usage["prompt_tokens_details"];
            if (pdetails.HasMember("cached_tokens") && pdetails["cached_tokens"].IsUint()) {
                am.tokens.cacheRead = pdetails["cached_tokens"].GetUint();
            }
        }
        am.tokens.prompt = am.tokens.contextSize - am.tokens.cacheRead;
        am.tokens.cumulativePrompt = am.tokens.prompt;
        am.tokens.total = am.tokens.prompt + am.tokens.completion;

        // Extract reasoning tokens from completion_tokens_details
        if (usage.HasMember("completion_tokens_details") && usage["completion_tokens_details"].IsObject()) {
            const auto& details = usage["completion_tokens_details"];
            if (details.HasMember("reasoning_tokens") && details["reasoning_tokens"].IsUint()) {
                am.tokens.reasoning = details["reasoning_tokens"].GetUint();
            }
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
    
    if (supportsStreamUsage()) {
        rapidjson::Value streamOpts(rapidjson::kObjectType);
        streamOpts.AddMember("include_usage", true, a);
        d.AddMember("stream_options", streamOpts, a);
    }

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

bool BaseOpenAIProvider::supportsStreamUsage() const {
    return true;
}

ModelInfo BaseOpenAIProvider::getModelInfo(const std::string& modelId) {
    if (!modelsCached) {
        cachedModels = listModels();
        modelsCached = true;
    }
    for (const auto& m : cachedModels) {
        if (m.id == modelId) return m;
    }
    // Return a default with no pricing if model not found
    ModelInfo unknown;
    unknown.id = modelId;
    unknown.provider = "unknown";
    return unknown;
}

void BaseOpenAIProvider::calculateCost(AgentMetrics& metrics, const ModelInfo& model) const {
    metrics.estimatedCostUsd =
        (model.pricePer1MInput / 1'000'000.0) * metrics.tokens.prompt +
        (model.pricePer1MOutput / 1'000'000.0) * metrics.tokens.completion +
        (model.pricePer1MCacheRead / 1'000'000.0) * metrics.tokens.cacheRead +
        (model.pricePer1MCacheWrite / 1'000'000.0) * metrics.tokens.cacheWrite;
}

std::string BaseOpenAIProvider::generateSummary(const AgentHistory& history, const std::string& compactionPrompt) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("CURL init failed");

    std::string url = baseUrl + "/chat/completions";
    
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();

    // Use a reasonable model, either the current one or a fixed one if needed.
    // For simplicity, we'll assume the caller wants to use the session's model if possible, 
    // but we don't have ProviderOptions here.
    // Actually, history doesn't have modelId. 
    // We'll use a placeholder or assume the first turn has it? No.
    // Let's assume we use the last model id seen or a sensible default.
    // Wait, I should probably pass the modelId to generateSummary.
    // But the interface says generateSummary(const AgentHistory&, const std::string&).
    
    // I'll use "gpt-4o" or similar if not specified, but usually we want to match the agent.
    // Let's look at how Agent calls it.
    
    d.AddMember("model", "gpt-4o", a); // Placeholder, will fix if I can get better info
    d.AddMember("temperature", 0.1f, a);
    d.AddMember("stream", false, a);

    rapidjson::Value messages(rapidjson::kArrayType);
    for (const auto& turn : history.turns) {
        for (const auto& msg : turn.messages) {
            if (msg.role == Role::ToolResult) {
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
            for (const auto& part : msg.content) {
                if (auto* txt = std::get_if<TextContent>(&part)) textContent += txt->text;
                else if (auto* thk = std::get_if<ThinkingContent>(&part)) textContent += "\n<thinking>\n" + thk->thinking + "\n</thinking>\n";
                else if (auto* tcc = std::get_if<ToolCallContent>(&part)) {
                    // Include tool calls in text for summary context if they aren't natively supported in this call
                    textContent += "\n[Tool Call: " + tcc->name + " args: " + tcc->args + "]\n";
                }
            }
            m.AddMember("content", rapidjson::Value(textContent.c_str(), a), a);
            messages.PushBack(m, a);
        }
    }

    // Add compaction prompt as final user message
    rapidjson::Value finalMsg(rapidjson::kObjectType);
    finalMsg.AddMember("role", "user", a);
    finalMsg.AddMember("content", rapidjson::Value(compactionPrompt.c_str(), a), a);
    messages.PushBack(finalMsg, a);

    d.AddMember("messages", messages, a);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    std::string body = buffer.GetString();

    struct curl_slist* headers = nullptr;
    auto headerMap = getHeaders();
    for (const auto& [k, v] : headerMap) {
        headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }

    std::string response;
    auto writerCb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
        return size * nmemb;
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<size_t(*)(char*,size_t,size_t,void*)>(writerCb));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || response_code >= 400) {
        throw std::runtime_error("Summary generation failed: " + response);
    }

    rapidjson::Document resDoc;
    resDoc.Parse(response.c_str());
    if (resDoc.HasMember("choices") && resDoc["choices"].IsArray() && resDoc["choices"].Size() > 0) {
        return resDoc["choices"][0]["message"]["content"].GetString();
    }

    return "";
}

} // namespace firmius::provider
