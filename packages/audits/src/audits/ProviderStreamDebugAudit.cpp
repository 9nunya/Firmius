#include "audits/ProviderStreamDebugAudit.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "providers/ProviderRegistry.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <atomic>

namespace firmius::audits {

using namespace firmius::provider;
using namespace firmius::shared;

namespace {
std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

std::string escapeString(const std::string& s) {
    std::string escaped;
    for (char c : s) {
        switch (c) {
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}
}

std::string ProviderStreamDebugAudit::getId() const { return "provider_stream_debug"; }

std::string ProviderStreamDebugAudit::getDescription() const { 
    return "Debug: Log EVERY chunk from provider stream to STDOUT"; 
}

shared::AuditResult ProviderStreamDebugAudit::run(const std::vector<std::string>& args) {
    AuditResult result;
    result.auditId = getId();
    
    if (args.empty()) {
        std::cerr << "Usage: firmius_audit --audit provider_stream_debug <provider_id> [model_id]" << std::endl;
        std::cerr << "Example: firmius_audit --audit provider_stream_debug qwen qwen3-coder-flash" << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
    }

    EnvLoader::load(".env.local");
    firmius::core::Engine::instance();
    
    std::string providerName = args[0];
    auto provider = ProviderRegistry::instance().getProvider(providerName);
    if (!provider) {
        std::cerr << "Unknown provider: " << providerName << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
    }

    std::cout << "=== Provider Stream Debug Audit ===" << std::endl;
    std::cout << "Provider: " << providerName << std::endl;
    
    auto models = provider->listModels();
    std::string modelId;
    if (args.size() > 1) {
        modelId = args[1];
    } else {
        // Pick a reasonable default
        for (const auto& m : models) {
            if (m.id.find("flash") != std::string::npos || 
                m.id.find("gpt-4") != std::string::npos ||
                m.id.find("claude") != std::string::npos) {
                modelId = m.id;
                break;
            }
        }
        if (modelId.empty() && !models.empty()) {
            modelId = models[0].id;
        }
    }
    
    if (modelId.empty()) {
        std::cerr << "No model available for provider" << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
    }
    
    std::cout << "Model: " << modelId << std::endl;
    std::cout << "====================================" << std::endl;
    std::cout << std::endl;

    // Simple prompt to trigger a response
    AgentHistory history;
    history.threadId = "debug-audit";
    AgentTurn turn;
    Message msg;
    msg.role = Role::User;
    msg.content.push_back(TextContent{"Say hello and tell me what model you are."});
    turn.messages.push_back(msg);
    history.turns.push_back(turn);

    ProviderOptions opts;
    opts.modelId = modelId;
    opts.temperature = 0.7f;
    opts.maxTokens = std::optional<int>(100);

    std::cout << "--- Stream Events (timestamp | type | data) ---" << std::endl;
    std::atomic<int> chunkCount{0};
    std::atomic<int> errorCount{0};
    
    provider->stream(history, opts, [&](const StreamEvent& ev) {
        std::string ts = getTimestamp();
        chunkCount++;
        
        if (auto* txt = std::get_if<TextChunk>(&ev)) {
            std::cout << "[" << ts << "] [TEXT] \"" << escapeString(txt->delta) << "\"" << std::endl;
        } else if (auto* thk = std::get_if<ThinkingChunk>(&ev)) {
            std::cout << "[" << ts << "] [THINKING] delta=\"" << escapeString(thk->delta) << "\" signature=\"" << escapeString(thk->signature) << "\"" << std::endl;
        } else if (auto* tcc = std::get_if<ToolCallChunk>(&ev)) {
            std::cout << "[" << ts << "] [TOOL_CHUNK] index=" << tcc->index << " id=" << tcc->id << " name=" << tcc->nameDelta << " args=" << escapeString(tcc->argsDelta) << std::endl;
        } else if (auto* met = std::get_if<AgentMetrics>(&ev)) {
            std::cout << "[" << ts << "] [METRICS] prompt=" << met->tokens.prompt 
                      << " completion=" << met->tokens.completion 
                      << " total=" << met->tokens.total
                      << " cache_read=" << met->tokens.cacheRead
                      << " cache_write=" << met->tokens.cacheWrite
                      << " reasoning=" << met->tokens.reasoning
                      << " context_size=" << met->tokens.contextSize << std::endl;
        } else if (auto* done = std::get_if<StreamDone>(&ev)) {
            std::string reasonStr;
            switch (done->reason) {
                case StopReason::Stop: reasonStr = "stop"; break;
                case StopReason::ToolUse: reasonStr = "tool_use"; break;
                case StopReason::MaxTokens: reasonStr = "max_tokens"; break;
                case StopReason::ContentFilter: reasonStr = "content_filter"; break;
                case StopReason::Error: reasonStr = "error"; break;
                case StopReason::Cancelled: reasonStr = "cancelled"; break;
            }
            std::cout << "[" << ts << "] [STREAM_DONE] reason=" << reasonStr << std::endl;
        } else if (auto* err = std::get_if<StreamError>(&ev)) {
            errorCount++;
            std::cout << "[" << ts << "] [STREAM_ERROR] httpStatus=" << err->httpStatus
                      << " account=" << err->accountLocator
                      << " message=\"" << err->message << "\"" << std::endl;
        } else if (auto* retry = std::get_if<StreamRetrying>(&ev)) {
            std::cout << "[" << ts << "] [STREAM_RETRYING] attempt=" << retry->attempt
                      << " of " << retry->maxAttempts
                      << " delay_ms=" << retry->delayMs
                      << " reason=\"" << retry->reason << "\"" << std::endl;
        } else if (auto* switched = std::get_if<StreamAccountSwitched>(&ev)) {
            std::cout << "[" << ts << "] [STREAM_ACCOUNT_SWITCHED] account=" << switched->accountLocator << std::endl;
        } else {
            std::cout << "[" << ts << "] [UNKNOWN_EVENT] variant_index=" << ev.index() << std::endl;
        }
        std::cout << std::flush;
    });

    std::cout << std::endl;
    std::cout << "=== Summary ===" << std::endl;
    std::cout << "Total events: " << chunkCount.load() << std::endl;
    std::cout << "Errors: " << errorCount.load() << std::endl;
    
    result.exitCode = errorCount.load() > 0 ? 1 : 0;
    result.passed = errorCount.load() == 0;
    return result;
}

} // namespace firmius::audits
