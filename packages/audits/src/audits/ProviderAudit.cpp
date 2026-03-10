#include "audits/ProviderAudit.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "providers/ProviderRegistry.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

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
}

std::string ProviderAudit::getId() const { return "provider_models"; }

std::string ProviderAudit::getDescription() const { return "List models and stream a tool call"; }

shared::AuditResult ProviderAudit::run(const std::vector<std::string>& args) {
    AuditResult result;
    result.auditId = getId();
    EnvLoader::load(".env.local");
    firmius::core::Engine::instance();
    std::string providerName = args.empty() ? "nanogpt" : args[0];
    auto provider = ProviderRegistry::instance().getProvider(providerName);
    if (!provider) {
        std::cerr << "Unknown provider: " << providerName << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
    }
    std::cout << "--- Models List for " << providerName << " ---" << std::endl;
    auto models = provider->listModels();
    std::cout << std::left << std::setw(40) << "ID" << " | " << std::setw(10) << "Context" << " | " << "Modalities" << std::endl;
    std::cout << std::string(70, '-') << std::endl;
    for (const auto& m : models) {
        std::string mods;
        for (const auto& mod : m.modalities) {
            mods += mod + " ";
        }
        std::cout << std::left << std::setw(40) << m.id << " | " << std::setw(10) << m.contextWindow << " | " << mods << std::endl;
    }
    std::cout << "\n--- Streaming Audit (Tool Call Latency) ---" << std::endl;
    AgentHistory history;
    history.threadId = "audit-thread";
    AgentTurn turn;
    Message msg;
    msg.role = Role::User;
    msg.content.push_back(TextContent{"Use file_read to check package.json. DO NOT SAY ANYTHING ELSE."});
    turn.messages.push_back(msg);
    history.turns.push_back(turn);
    ProviderOptions opts;
    opts.tools = {
        {"file_read", "Read a file from the filesystem", R"({"type":"object","properties":{"path":{"type":"string"}}})"}
    };
    if (args.size() > 1) {
        opts.modelId = args[1];
    } else {
        opts.modelId = models.empty() ? "gpt-4o" : models[0].id;
        if (providerName == "nanogpt") {
            for (const auto& m : models) {
                if (m.id.find("gpt-4") != std::string::npos || m.id.find("claude-3") != std::string::npos) {
                    opts.modelId = m.id;
                    break;
                }
            }
        }
    }
    std::cout << "Using model: " << opts.modelId << std::endl;
    provider->stream(history, opts, [&](const StreamEvent& ev) {
        std::string ts = getTimestamp();
        if (auto* txt = std::get_if<TextChunk>(&ev)) {
            std::cout << "[" << ts << "] TEXT: " << txt->delta << std::endl;
        } else if (auto* thk = std::get_if<ThinkingChunk>(&ev)) {
            std::cout << "[" << ts << "] THINKING: " << thk->delta << std::endl;
        } else if (auto* tcc = std::get_if<ToolCallChunk>(&ev)) {
            std::cout << "[" << ts << "] TOOL_CHUNK: id=" << tcc->id << " name=" << tcc->nameDelta << " args=" << tcc->argsDelta << std::endl;
        } else if (auto* met = std::get_if<AgentMetrics>(&ev)) {
            std::cout << "[" << ts << "] METRICS: total_tokens=" << met->tokens.total << std::endl;
        }
    });
    result.exitCode = 0;
    result.passed = true;
    return result;
}

}
