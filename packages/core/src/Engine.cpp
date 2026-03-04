#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
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
#include "persistence/HistoryEditor.hpp"
#include "hosts/LocalHost.hpp"
#include "hosts/DockerHost.hpp"
#include "tools/FileReadTool.hpp"
#include "tools/FileEditTool.hpp"
#include "tools/ProcessExecuteTool.hpp"
#include "tools/SubagentTool.hpp"
#include "tools/SubagentWaitTool.hpp"
#include "tools/SubagentTerminateTool.hpp"
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
#include <Panic.hpp>
#include <iostream>
#include <future>
#include <algorithm>
#include <sstream>

namespace firmius::core {

Engine::Engine() {
    initProviders();

    shared::Panic::addExtraInfo("active_agents", []() -> std::string {
        std::stringstream ss;
        auto agentIds = AgentRegistry::instance().listAll();
        ss << "Count: " << agentIds.size() << "\n";
        for (const auto& id : agentIds) {
            auto agent = AgentRegistry::instance().getAgent(id);
            if (agent) {
                const auto& ctx = agent->getContext();
                ss << "  - " << id << ": "
                   << ctx.identity.name << " ("
                   << (agent->isRunning() ? "running" : "idle") << ", "
                   << ctx.history.threadId << ")\n";
            }
        }
        return ss.str();
    });

    toolRegistry.registerTool(std::make_unique<FileReadTool>());
    toolRegistry.registerTool(std::make_unique<FileEditTool>());
    toolRegistry.registerTool(std::make_unique<ProcessExecuteTool>());
    toolRegistry.registerTool(std::make_unique<SubagentTool>());
    toolRegistry.registerTool(std::make_unique<SubagentTerminateTool>());
    toolRegistry.registerTool(std::make_unique<SubagentWaitTool>());
    toolRegistry.registerTool(std::make_unique<PythonExecuteTool>());
    toolRegistry.registerTool(std::make_unique<ListDirectoryTool>());
    toolRegistry.registerTool(std::make_unique<GlobTool>());
    toolRegistry.registerTool(std::make_unique<GrepTool>());
    toolRegistry.registerTool(std::make_unique<WebFetchTool>());
    toolRegistry.registerTool(std::make_unique<ProcessSpawnTool>());
    toolRegistry.registerTool(std::make_unique<ProcessStatusTool>());
    toolRegistry.registerTool(std::make_unique<ProcessWaitTool>());
    toolRegistry.registerTool(std::make_unique<ProcessInputTool>());
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

std::string Engine::summonAgent(const std::string& threadId, const std::string& personaName,
                                const std::string& task, bool persistHistory,
                                const std::string& parentId, const std::string& friendlyName,
                                const std::string& title) {
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

    // Create agent and register BEFORE spawning thread to avoid race condition
    std::unique_ptr<IHost> host;
    try {
        auto metadata = ThreadManager::getMetadata(threadId);
        auto persona = PurposeLoader::load(personaName);

        AgentContext ctx;
        ctx.identity.id = agentId;
        ctx.identity.parentId = parentId;
        ctx.identity.friendlyName = parentId.empty() ? "lead" : friendlyName;
        const auto& userCfg = shared::ConfigLoader::instance().getConfig();
        ctx.config.providerId = userCfg.defaultProviderId;
        ctx.config.modelId = userCfg.defaultModelId;
        ctx.config.temperature = userCfg.defaultTemperature;
        if (userCfg.defaultMaxTokens.has_value()) {
            ctx.config.maxTokens = userCfg.defaultMaxTokens.value();
        }
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
            host = std::make_unique<DockerHost>();
        } else {
            host = std::make_unique<LocalHost>();
        }
        std::string actualHostId = host->init();
        
        if (metadata.hostIdentifier != actualHostId) {
            ThreadManager::updateHostIdentifier(threadId, actualHostId);
            metadata.hostIdentifier = actualHostId;
            ctx.environment.identifier = actualHostId;
        }

        std::shared_ptr<Journaler> jnl = nullptr;
        if (ctx.config.persistHistory) {
            jnl = std::make_shared<Journaler>(threadId, agentId);
        }

        auto agent = std::make_shared<Agent>(ctx, std::move(host), toolRegistry, jnl);

        // Register BEFORE spawning thread - ensures agent is available immediately
        AgentRegistry::instance().registerAgent(agentId, agent);
        std::string agentTitle = title.empty() ? persona.title : title;
        broadcast(AgentSpawned{agentId, personaName, parentId, ctx.identity.friendlyName, agentTitle, persistHistory});

    } catch (...) {
        prom->set_exception(std::current_exception());
        return agentId;
    }

    std::lock_guard<std::mutex> lock(listenerMutex);
    fleet.emplace_back([this, threadId, agentId, personaName, task, prom, persistHistory, parentId]() {
        try {
            auto agent = AgentRegistry::instance().getAgent(agentId);
            if (!agent) {
                throw std::runtime_error("Agent not found in registry");
            }

            std::string finalSummary = "No summary provided.";

            agent->run(task, [this, agentId, parentId](const StreamEvent& ev) {
                if (auto* txt = std::get_if<TextChunk>(&ev)) {
                    broadcast(AgentText{agentId, txt->delta, parentId});
                } else if (auto* thk = std::get_if<ThinkingChunk>(&ev)) {
                    broadcast(AgentThinking{agentId, thk->delta, parentId});
                } else if (auto* tcc = std::get_if<ToolCallChunk>(&ev)) {
                    broadcast(AgentToolCall{agentId, tcc->id, tcc->nameDelta, tcc->argsDelta, parentId});
                } else if (auto* tc = std::get_if<AgentTurnCompleted>(&ev)) {
                    broadcast(*tc);
                } else if (auto* ac = std::get_if<AgentCompacting>(&ev)) {
                    broadcast(*ac);
                } else if (auto* act = std::get_if<AgentCompactionThinking>(&ev)) {
                    broadcast(*act);
                } else if (auto* acx = std::get_if<AgentCompactionText>(&ev)) {
                    broadcast(*acx);
                } else if (auto* cc = std::get_if<ContextCompacted>(&ev)) {
                    broadcast(*cc);
                } else if (auto* pod = std::get_if<ProcessOutputDelta>(&ev)) {
                    broadcast(AgentProcessOutput{agentId, pod->processId, pod->output, pod->isStderr, pod->finished, parentId});
                } else if (auto* sr = std::get_if<StreamRetrying>(&ev)) {
                    broadcast(AgentRetrying{agentId, sr->attempt, sr->maxAttempts, sr->httpStatus, sr->delayMs, sr->reason, parentId});
                } else if (auto* sre = std::get_if<StreamRetryExhausted>(&ev)) {
                    broadcast(AgentRetryFailed{agentId, sre->httpStatus, sre->reason, parentId});
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

            broadcast(AgentCompleted{agentId, finalSummary, parentId});
            prom->set_value(finalSummary);

        } catch (const std::exception& e) {
            broadcast(AgentError{agentId, e.what(), parentId});
            prom->set_exception(std::make_exception_ptr(e));
        }
    });

    return agentId;
}

std::string Engine::resumeAgent(const std::string& threadId, const std::string& agentId,
                                const std::string& personaName, const std::string& parentId,
                                const std::string& friendlyName, const std::string& title,
                                bool persistHistory) {
    reap();

    if (AgentRegistry::instance().getAgent(agentId)) {
        throw std::runtime_error("Agent already exists: " + agentId);
    }

    auto metadata = ThreadManager::getMetadata(threadId);
    auto persona = PurposeLoader::load(personaName);
    auto history = ThreadManager::loadAgentHistory(threadId, agentId);

    AgentContext ctx;
    ctx.identity.id = agentId;
    ctx.identity.parentId = parentId;
    ctx.identity.friendlyName = parentId.empty() ? "lead" : friendlyName;
    ctx.identity.name = persona.name;
    ctx.identity.role = title.empty() ? persona.title : title;
    const auto& userCfg = shared::ConfigLoader::instance().getConfig();
    ctx.config.providerId = userCfg.defaultProviderId;
    ctx.config.modelId = userCfg.defaultModelId;
    ctx.config.temperature = userCfg.defaultTemperature;
    if (userCfg.defaultMaxTokens.has_value()) {
        ctx.config.maxTokens = userCfg.defaultMaxTokens.value();
    }
    ctx.config.persistHistory = persistHistory;
    ctx.permissions.allowedScopes = persona.allowedScopes;
    ctx.permissions.allowedPaths = {metadata.cwd, "/tmp"};
    ctx.environment.cwd = metadata.cwd;
    ctx.environment.identifier = metadata.hostIdentifier;
    ctx.environment.type = metadata.hostType;
    ctx.history = std::move(history);

    std::unique_ptr<IHost> host;
    if (metadata.hostType == HostType::Docker) {
        host = std::make_unique<DockerHost>();
    } else {
        host = std::make_unique<LocalHost>();
    }
    std::string actualHostId = host->init();
    if (metadata.hostIdentifier != actualHostId) {
        ThreadManager::updateHostIdentifier(threadId, actualHostId);
        ctx.environment.identifier = actualHostId;
    }

    std::shared_ptr<Journaler> jnl = nullptr;
    if (ctx.config.persistHistory) {
        jnl = std::make_shared<Journaler>(threadId, agentId);
    }

    auto agent = std::make_shared<Agent>(ctx, std::move(host), toolRegistry, jnl);
    AgentRegistry::instance().registerAgent(agentId, agent);

    std::string agentTitle = title.empty() ? persona.title : title;
    broadcast(AgentSpawned{agentId, personaName, parentId, ctx.identity.friendlyName, agentTitle, persistHistory});

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

void Engine::terminateAgent(const std::string& agentId) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (agent) {
        // Unregister from registry - this removes the shared_ptr reference
        // When all references are gone, the Agent destructor will be called
        // which will destroy the host (cleanup Docker containers, etc.)
        AgentRegistry::instance().unregisterAgent(agentId);
    }
}

void Engine::switchAgentModel(const std::string& agentId, const std::string& providerId, const std::string& modelId) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (!agent) {
        throw std::runtime_error("Agent not found: " + agentId);
    }

    std::string oldProviderId = agent->getContext().config.providerId;
    std::string oldModelId = agent->getContext().config.modelId;

    agent->setModel(providerId, modelId);

    broadcast(ModelSwitched{agentId, oldProviderId, oldModelId, providerId, modelId, agent->getContext().identity.parentId});
}

void Engine::executeTask(const std::string& agentId, const std::string& task) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (!agent) {
        throw std::runtime_error("Agent not found: " + agentId);
    }

    auto prom = std::make_shared<std::promise<std::string>>();
    {
        std::lock_guard<std::mutex> lock(futuresMutex);
        agentFutures[agentId] = prom->get_future().share();
    }

    std::thread([this, agentId, task, agent, prom]() mutable {
        std::string parentId = "";
        try {
            if (agent) {
                parentId = agent->getContext().identity.parentId;
            }
            std::string finalSummary = "No summary provided.";

            agent->run(task, [this, agentId, parentId](const StreamEvent& ev) {
                if (auto* txt = std::get_if<TextChunk>(&ev)) {
                    broadcast(AgentText{agentId, txt->delta, parentId});
                } else if (auto* thk = std::get_if<ThinkingChunk>(&ev)) {
                    broadcast(AgentThinking{agentId, thk->delta, parentId});
                } else if (auto* tcc = std::get_if<ToolCallChunk>(&ev)) {
                    broadcast(AgentToolCall{agentId, tcc->id, tcc->nameDelta, tcc->argsDelta, parentId});
                } else if (auto* tc = std::get_if<AgentTurnCompleted>(&ev)) {
                    broadcast(*tc);
                } else if (auto* ac = std::get_if<AgentCompacting>(&ev)) {
                    broadcast(*ac);
                } else if (auto* act = std::get_if<AgentCompactionThinking>(&ev)) {
                    broadcast(*act);
                } else if (auto* acx = std::get_if<AgentCompactionText>(&ev)) {
                    broadcast(*acx);
                } else if (auto* cc = std::get_if<ContextCompacted>(&ev)) {
                    broadcast(*cc);
                } else if (auto* pod = std::get_if<ProcessOutputDelta>(&ev)) {
                    broadcast(AgentProcessOutput{agentId, pod->processId, pod->output, pod->isStderr, pod->finished, parentId});
                } else if (auto* sr = std::get_if<StreamRetrying>(&ev)) {
                    broadcast(AgentRetrying{agentId, sr->attempt, sr->maxAttempts, sr->httpStatus, sr->delayMs, sr->reason, parentId});
                } else if (auto* sre = std::get_if<StreamRetryExhausted>(&ev)) {
                    broadcast(AgentRetryFailed{agentId, sre->httpStatus, sre->reason, parentId});
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

            broadcast(AgentCompleted{agentId, finalSummary, parentId});
            prom->set_value(finalSummary);

        } catch (const std::exception& e) {
            broadcast(AgentError{agentId, e.what(), parentId});
            prom->set_exception(std::make_exception_ptr(e));
        }
    }).detach();
}

UndoResult Engine::undoAgentTurns(const std::string& agentId, int count) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (!agent) throw std::runtime_error("Agent not found: " + agentId);
    if (agent->isRunning()) throw std::runtime_error("Cannot undo while agent is running");

    auto& ctx = agent->getMutableContext();
    auto result = HistoryEditor::undoTurns(ctx.history.turns, count);

    broadcast(HistoryUndone{agentId, ctx.history.threadId, result.turnsRemoved, result.compactionReversed, ctx.identity.parentId});
    return result;
}

UndoResult Engine::undoAgentMessages(const std::string& agentId, int count) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (!agent) throw std::runtime_error("Agent not found: " + agentId);
    if (agent->isRunning()) throw std::runtime_error("Cannot undo while agent is running");

    auto& ctx = agent->getMutableContext();
    auto result = HistoryEditor::undoMessages(ctx.history.turns, count);

    broadcast(HistoryUndone{agentId, ctx.history.threadId, result.turnsRemoved, result.compactionReversed, ctx.identity.parentId});
    return result;
}

UndoResult Engine::undoAgentAfterTimestamp(const std::string& agentId, uint64_t timestamp) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (!agent) throw std::runtime_error("Agent not found: " + agentId);
    if (agent->isRunning()) throw std::runtime_error("Cannot undo while agent is running");

    auto& ctx = agent->getMutableContext();
    auto result = HistoryEditor::undoAfterTimestamp(ctx.history.turns, timestamp);

    broadcast(HistoryUndone{agentId, ctx.history.threadId, result.turnsRemoved, result.compactionReversed, ctx.identity.parentId});
    return result;
}

}
