#include "EnvLoader.hpp"
#include "ConcreteProviders.hpp"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

using namespace firmius::shared;
using namespace firmius::provider;
using namespace firmius::shared;

std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

int main(int argc, char** argv) {
    EnvLoader::load(".env.local");

    std::string providerName = argc > 1 ? argv[1] : "nanogpt";
    std::unique_ptr<IProvider> provider;

    if (providerName == "nanogpt") {
        provider = std::make_unique<NanoGPTProvider>(std::vector<std::string>{});
    } else if (providerName == "openrouter") {
        provider = std::make_unique<OpenRouterProvider>(EnvLoader::get("OPENROUTER_API_KEY"));
    } else if (providerName == "zai") {
        provider = std::make_unique<ZaiProvider>(EnvLoader::get("ZAI_API_KEY"));
    } else if (providerName == "zen") {
        provider = std::make_unique<ZenProvider>(EnvLoader::get("ZEN_API_KEY"));
    } else {
        std::cerr << "Unknown provider: " << providerName << std::endl;
        return 1;
    }

    std::cout << "--- Models List for " << providerName << " ---" << std::endl;
    auto models = provider->listModels();
    std::cout << std::left << std::setw(40) << "ID" << " | " << std::setw(10) << "Context" << " | " << "Modalities" << std::endl;
    std::cout << std::string(70, '-') << std::endl;
    for (const auto& m : models) {
        std::string mods;
        for (const auto& mod : m.modalities) mods += mod + " ";
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
    if (argc > 2) {
        opts.modelId = argv[2];
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

    provider->stream(history, opts, [](const StreamEvent& ev) {
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

    return 0;
}
