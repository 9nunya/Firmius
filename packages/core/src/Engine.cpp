#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "agents/Agent.hpp"
#include "agents/PurposeLoader.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/NanoGPTProvider.hpp"
#include "providers/OpenRouterProvider.hpp"
#include "providers/ZaiProvider.hpp"
#include "providers/ZenProvider.hpp"
#include "providers/ChutesProvider.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"
#include "hosts/LocalHost.hpp"
#include "hosts/DockerHost.hpp"
#include "tools/FileReadTool.hpp"
#include "tools/FileEditTool.hpp"
#include "tools/ProcessExecuteTool.hpp"
#include "tools/SubagentTool.hpp"
#include "tools/SubagentWaitTool.hpp"
#include "tools/PythonExecuteTool.hpp"
#include "tools/ListDirectoryTool.hpp"
#include "tools/GlobTool.hpp"
#include "tools/GrepTool.hpp"
#include "tools/WebFetchTool.hpp"
#include "tools/ProcessSpawnTool.hpp"
#include "tools/ProcessStatusTool.hpp"
#include "tools/ProcessWaitTool.hpp"
#include "tools/ProcessInputTool.hpp"
#include "utils/StringUtil.hpp"
#include <iostream>
#include <future>
#include <algorithm>

namespace firmius::core {

Engine::Engine() {
    initProviders();
}

void Engine::initProviders() {
    auto& reg = firmius::provider::ProviderRegistry::instance();
    reg.registerProvider(std::make_shared<firmius::provider::NanoGPTProvider>());
    reg.registerProvider(std::make_shared<firmius::provider::OpenRouterProvider>(""));
    reg.registerProvider(std::make_shared<firmius::provider::ZaiProvider>(""));
    reg.registerProvider(std::make_shared<firmius::provider::ZenProvider>(""));
    reg.registerProvider(std::make_shared<firmius::provider::ChutesProvider>(""));
}

void Engine::reap() {
    std::lock_guard<std::mutex> lock(listenerMutex);
    // std::jthread cleans up itself but we should remove finished ones from our list
    // Actually fleet is std::vector<std::jthread>, so we can just check if they are finished.
    // However jthread doesn't have an easy "is_finished" but we can check if it's joinable.
    // But jthread is always joinable until joined.
    // A better way is to use a map of futures or similar.
    // For now, let's just leave them in fleet as they are jthreads.
}

std::string Engine::summonAgent(const std::string& threadId, const std::string& personaName, const std::string& task, bool persistHistory) {
    reap();

    if (AgentRegistry::instance().listAll().size() >= maxConcurrentAgents) {
        throw std::runtime_error("Maximum concurrent agents reached");
    }

    std::string agentId = shared::StringUtil::generateUuid();
    
    auto prom = std::make_shared<std::promise<std::string>>();
    {
        std::lock_guard<std::mutex> lock(futuresMutex);
        agentFutures[agentId] = prom->get_future().share();
    }

    std::lock_guard<std::mutex> lock(listenerMutex);
    fleet.emplace_back([this, threadId, agentId, personaName, task, prom, persistHistory]() {
        std::unique_ptr<IHost> host;
        try {
            auto metadata = ThreadManager::getMetadata(threadId);
            auto persona = PurposeLoader::load(personaName);
            
            AgentContext ctx;
            ctx.identity.id = agentId;
            ctx.config.modelId = "zai-org/glm-4.7:thinking";
            ctx.config.persistHistory = persistHistory;
            ctx.identity.name = persona.name;
            ctx.identity.role = persona.title;
            ctx.permissions.allowedScopes = persona.allowedScopes;
            ctx.permissions.allowedPaths = {metadata.cwd, "/tmp"};
            ctx.environment.cwd = metadata.cwd;
            ctx.environment.identifier = metadata.hostIdentifier;
            ctx.environment.type = metadata.hostType;
            ctx.history.threadId = threadId;

            if (metadata.hostType == HostType::Docker) {
                host = std::make_unique<DockerHost>(metadata.hostIdentifier);
            } else {
                host = std::make_unique<LocalHost>();
            }
            host->init();

            ToolRegistry reg;
            reg.registerTool(std::make_unique<FileReadTool>());
            reg.registerTool(std::make_unique<FileEditTool>());
            reg.registerTool(std::make_unique<ProcessExecuteTool>());
            reg.registerTool(std::make_unique<SubagentTool>());
            reg.registerTool(std::make_unique<SubagentWaitTool>());
            reg.registerTool(std::make_unique<PythonExecuteTool>());
            reg.registerTool(std::make_unique<ListDirectoryTool>());
            reg.registerTool(std::make_unique<GlobTool>());
            reg.registerTool(std::make_unique<GrepTool>());
            reg.registerTool(std::make_unique<WebFetchTool>());
            reg.registerTool(std::make_unique<ProcessSpawnTool>());
            reg.registerTool(std::make_unique<ProcessStatusTool>());
            reg.registerTool(std::make_unique<ProcessWaitTool>());
            reg.registerTool(std::make_unique<ProcessInputTool>());

            std::shared_ptr<Journaler> jnl = nullptr;
            if (ctx.config.persistHistory) {
                jnl = std::make_shared<Journaler>(threadId, agentId);
            }

            auto agent = std::make_shared<Agent>(ctx, *host, reg, jnl);
            
            AgentRegistry::instance().registerAgent(agentId, agent);

            broadcast(AgentSpawned{agentId, personaName});

            std::string finalSummary = "No summary provided.";

            agent->run(task, [this, agentId](const StreamEvent& ev) {
                if (auto* txt = std::get_if<TextChunk>(&ev)) {
                    broadcast(AgentText{agentId, txt->delta});
                } else if (auto* thk = std::get_if<ThinkingChunk>(&ev)) {
                    broadcast(AgentThinking{agentId, thk->delta});
                } else if (auto* tcc = std::get_if<ToolCallChunk>(&ev)) {
                    broadcast(AgentToolCall{agentId, tcc->nameDelta, tcc->argsDelta});
                } else if (auto* tc = std::get_if<AgentTurnCompleted>(&ev)) {
                    broadcast(*tc);
                }
            });

            // Extract last message for summary
            const auto& turns = agent->getContext().history.turns;
            if (!turns.empty() && !turns.back().messages.empty()) {
                const auto& lastMsg = turns.back().messages.back();
                std::string content;
                for (const auto& part : lastMsg.content) {
                    if (auto* txt = std::get_if<TextContent>(&part)) content += txt->text;
                }
                if (!content.empty()) finalSummary = content;
            }

            broadcast(AgentCompleted{agentId, finalSummary});
            prom->set_value(finalSummary);
            
        } catch (const std::exception& e) {
            broadcast(AgentError{agentId, e.what()});
            prom->set_exception(std::make_exception_ptr(e));
        }

        // Cleanup block
        AgentRegistry::instance().unregisterAgent(agentId);
        if (host) host->destroy();
    });

    return agentId;
}

std::string Engine::waitForAgent(const std::string& agentId) {
    std::shared_future<std::string> fut;
    {
        std::lock_guard<std::mutex> lock(futuresMutex);
        auto it = agentFutures.find(agentId);
        if (it == agentFutures.end()) return "Error: Agent not found or already waited on.";
        fut = it->second;
    }
    
    try {
        std::string res = fut.get();
        std::lock_guard<std::mutex> lock(futuresMutex);
        agentFutures.erase(agentId);
        return res;
    } catch (const std::exception& e) {
        return "Error: " + std::string(e.what());
    }
}

void Engine::addEventListener(std::function<void(const EngineEvent&)> listener) {
    std::lock_guard<std::mutex> lock(listenerMutex);
    listeners.push_back(listener);
}

void Engine::cancelAgent(const std::string& agentId) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (agent) {
        agent->interrupt();
    }
}

std::vector<std::string> Engine::listActiveAgents() const {
    return AgentRegistry::instance().listAll();
}

void Engine::broadcast(const EngineEvent& event) {
    std::lock_guard<std::mutex> lock(listenerMutex);
    for (const auto& listener : listeners) {
        listener(event);
    }
}

}
