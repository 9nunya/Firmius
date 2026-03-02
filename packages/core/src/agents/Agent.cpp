#include "agents/Agent.hpp"
#include "agents/PurposeLoader.hpp"
#include "providers/ProviderRegistry.hpp"
#include "persistence/Journaler.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include "Message.hpp"
#include "Events.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include <iostream>
#include <chrono>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <cstdlib>
#include <algorithm>
#include <filesystem>

namespace firmius::core {

using namespace firmius::shared;

std::uint64_t Agent::nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

Agent::Agent(AgentContext ctx, shared::IHost& h, ToolRegistry& reg, std::shared_ptr<Journaler> jnl)
    : context(std::move(ctx)), host(h), toolRegistry(reg), journaler(jnl) {
    provider = firmius::provider::ProviderRegistry::instance().getProvider(context.config.providerId);
    if (!provider) throw std::runtime_error("Unknown provider: " + context.config.providerId);

    debugPrettyPrint = (std::getenv("FIRMIUS_PRETTY_PRINT") != nullptr);

    if (debugPrettyPrint) {
        std::cout << "AGENT INITIATED! INFO:\n";
        std::cout << "THREAD ID: " << context.history.threadId << "\n";
        std::cout << "PURPOSE: " << context.identity.role << "\n";
        std::cout << "PROVIDER: " << context.config.providerId << "\n";
        std::cout << "MODEL: " << context.config.modelId << "\n\n";
    }
}

Agent::~Agent() {
    for (const auto& id : backgroundProcessIds) {
        try {
            host.killBackgroundProcess(id);
        } catch (...) {}
    }
}

void Agent::reset() {
    context.history.turns.clear();
    context.aggregateMetrics = {};
    context.state = {};
    interrupted = false;
    running = false;
    
    for (const auto& id : backgroundProcessIds) {
        try {
            host.killBackgroundProcess(id);
        } catch (...) {}
    }
    backgroundProcessIds.clear();
}

std::string Agent::resolvePath(const std::string& inputPath) const {
    return shared::FSUtil::resolvePath(inputPath, context.environment.cwd);
}

void Agent::interrupt() {
    interrupted = true;
}

std::string Agent::spawnProcess(const std::string& command, const std::string& cwd, const std::map<std::string, std::string>& env) {
    auto proc = host.spawn(command, cwd, env);
    std::string id = host.registerBackgroundProcess(std::move(proc));
    backgroundProcessIds.insert(id);
    return id;
}

shared::ProcessSnapshot Agent::inspectProcess(const std::string& id) {
    return host.inspectBackgroundProcess(id);
}

void Agent::writeToProcess(const std::string& id, const std::string& data) {
    host.writeToBackgroundProcess(id, data);
}

void Agent::registerProcessId(const std::string& id) {
    backgroundProcessIds.insert(id);
}

void Agent::run(const std::string& task, std::function<void(const shared::StreamEvent&)> onEvent) {
    // Guard against concurrent runs
    if (running.load()) {
        throw std::runtime_error("Agent is already running");
    }
    running = true;
    interrupted = false;
    context.state.currentStatus = AgentStatus::Idle;
    context.state.fatalError = std::nullopt;

    // 1. Bootstrap System Message
    if (context.history.turns.empty()) {
        auto toolDefs = toolRegistry.getAvailableToolDefinitions(context.permissions);
        std::string personaName = context.config.personaName.empty() ? "coder" : context.config.personaName;
        Persona persona = PurposeLoader::load(personaName);
        std::string toolBlock = PurposeLoader::buildToolsBlock(toolDefs);

        std::string protocolAddon = "\n\n# PROTOCOL STRICTNESS\n"
                                    "- If you are calling a tool, your message MUST contain ONLY the tool call JSON.\n"
                                    "- Do NOT include any other text when calling a tool.\n"
                                    "- The <done /> token is NOT for when your turn is over.\n"
                                    "- Use <done /> ONLY when the ACTUAL FULL TASK requested by the user is completed.\n"
                                    "- Summarize your work before providing the <done /> token.\n";

        std::string systemPrompt = PurposeLoader::composeSystemPrompt(persona, context, toolBlock) + protocolAddon;

        AgentTurn turn;
        turn.turnId = "bootstrap-system";
        Message msg;
        msg.role = Role::System;
        msg.content.push_back(TextContent{systemPrompt});
        auto now = std::chrono::system_clock::now();
        msg.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        turn.messages.push_back(msg);
        context.history.turns.push_back(turn);
        if (context.config.persistHistory && journaler) journaler->appendTurn(turn);
    }

    // 2. Add Task as User Message
    bool hasTask = false;
    for (const auto& turn : context.history.turns) {
        for (const auto& msg : turn.messages) {
            if (msg.role == Role::User) {
                if (!msg.content.empty()) {
                    if (auto* txt = std::get_if<TextContent>(&msg.content[0])) {
                        if (txt->text.find("Please provide the <done /> tag") == std::string::npos) {
                            hasTask = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!hasTask) {
        AgentTurn taskTurn;
        taskTurn.turnId = "user-task-" + std::to_string(context.history.turns.size());
        Message taskMsg;
        taskMsg.role = Role::User;
        taskMsg.content.push_back(TextContent{task});
        auto now = std::chrono::system_clock::now();
        taskMsg.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        taskTurn.messages.push_back(taskMsg);
        context.history.turns.push_back(taskTurn);
        if (context.config.persistHistory && journaler) journaler->appendTurn(taskTurn);
    }

    // 3. Autonomous Loop
    bool taskFinished = false;
    int maxTurns = context.config.maxTurns > 0 ? context.config.maxTurns : 200;
    int turnCount = 0;

    while (!taskFinished && turnCount < maxTurns && !interrupted.load()) {
        // --- CHECK FOR CONTEXT COMPACTION ---
        try {
            auto model = provider->getModelInfo(context.config.modelId);
            if (model.contextWindow > 0 && context.aggregateMetrics.tokens.contextSize > model.contextWindow * 0.8) {
                compactContext(onEvent);
            }
        } catch (...) {
            // Compaction is best-effort
        }

        turnCount++;

        try {
            // --- State: Streaming ---
            context.state.currentStatus = AgentStatus::Streaming;

            firmius::provider::ProviderOptions opts;
            opts.modelId = context.config.modelId;
            opts.temperature = context.config.temperature;
            if (context.config.maxTokens.has_value()) {
                opts.maxTokens = context.config.maxTokens;
            }
            opts.stop = context.config.stop;
            opts.tools = toolRegistry.getAvailableToolDefinitions(context.permissions);

            std::vector<ToolCallChunk> accumulatedToolChunks;
            std::string fullResponse;
            std::string fullThinking;
            AgentMetrics turnMetrics;
            StopReason turnStopReason = StopReason::Stop;
            std::string streamError;

            if (debugPrettyPrint) {
                std::cout << "\n\033[1;36m--- Agent Turn " << turnCount
                          << " (context: " << context.aggregateMetrics.tokens.contextSize
                          << ", total billed: " << context.aggregateMetrics.tokens.total
                          << ", cost: $" << context.aggregateMetrics.estimatedCostUsd
                          << ") ---\033[0m\n";
            }

            provider->stream(context.history, opts, [&](const StreamEvent& ev) {
                onEvent(ev);

                if (auto* txt = std::get_if<TextChunk>(&ev)) {
                    fullResponse += txt->delta;
                    if (debugPrettyPrint) std::cout << txt->delta << std::flush;
                } else if (auto* thk = std::get_if<ThinkingChunk>(&ev)) {
                    fullThinking += thk->delta;
                    if (debugPrettyPrint) std::cout << "\033[3;37m" << thk->delta << "\033[0m" << std::flush;
                } else if (auto* tcc = std::get_if<ToolCallChunk>(&ev)) {
                    bool found = false;
                    bool argsStarted = false;
                    for (auto& existing : accumulatedToolChunks) {
                        if (existing.index == tcc->index) {
                            if (existing.argsDelta.empty() && !tcc->argsDelta.empty()) {
                                argsStarted = true;
                            }
                            existing.nameDelta += tcc->nameDelta;
                            existing.argsDelta += tcc->argsDelta;
                            if (!tcc->id.empty()) existing.id = tcc->id;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        auto newChunk = *tcc;
                        if (newChunk.id.empty()) newChunk.id = "call_" + std::to_string(turnCount) + "_" + std::to_string(tcc->index);
                        if (!newChunk.argsDelta.empty()) argsStarted = true;
                        accumulatedToolChunks.push_back(newChunk);
                    }

                    if (debugPrettyPrint) {
                        if (!found) {
                            std::cout << "\n\033[1;33m[Tool Call: " << tcc->nameDelta << std::flush;
                        } else if (!tcc->nameDelta.empty()) {
                            std::cout << tcc->nameDelta << std::flush;
                        }
                        if (argsStarted) {
                            std::cout << " (preparing)]\033[0m" << std::flush;
                        }
                    }
                } else if (auto* met = std::get_if<AgentMetrics>(&ev)) {
                    turnMetrics = *met;
                } else if (auto* done = std::get_if<StreamDone>(&ev)) {
                    turnStopReason = done->reason;
                } else if (auto* err = std::get_if<StreamError>(&ev)) {
                    streamError = err->message;
                }
            });

            if (debugPrettyPrint) std::cout << "\n";

            // If there was a stream error and no content came back, treat as error
            if (!streamError.empty() && fullResponse.empty() && accumulatedToolChunks.empty()) {
                throw std::runtime_error("Provider stream error: " + streamError);
            }

            // --- Build assistant turn ---
            AgentTurn assistantTurn;
            assistantTurn.turnId = "assistant-" + std::to_string(context.history.turns.size());
            assistantTurn.stopReason = turnStopReason;

            // Store per-turn metrics
            assistantTurn.metrics = turnMetrics;

            // Accumulate into session total
            context.aggregateMetrics += turnMetrics;

            Message assistantMsg;
            assistantMsg.role = Role::Assistant;
            if (!fullThinking.empty()) assistantMsg.content.push_back(ThinkingContent{fullThinking});
            if (!fullResponse.empty()) assistantMsg.content.push_back(TextContent{fullResponse});

            for (const auto& chunk : accumulatedToolChunks) {
                assistantMsg.content.push_back(ToolCallContent{chunk.id, chunk.nameDelta, chunk.argsDelta});
            }

            auto now_end = std::chrono::system_clock::now();
            assistantMsg.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now_end.time_since_epoch()).count());

            assistantTurn.messages.push_back(assistantMsg);
            context.history.turns.push_back(assistantTurn);
            if (context.config.persistHistory && journaler) journaler->appendTurn(assistantTurn);

            // Broadcast turn completion
            onEvent(AgentTurnCompleted{context.identity.id, assistantTurn, context.aggregateMetrics});

            // --- Check for termination ---
            if (accumulatedToolChunks.empty() && fullResponse.find("<done />") != std::string::npos) {
                taskFinished = true;
            } else if (accumulatedToolChunks.empty()) {
                // Nudge
                AgentTurn nudgeTurn;
                nudgeTurn.turnId = "nudge-" + std::to_string(turnCount);
                Message nudgeMsg;
                nudgeMsg.role = Role::User;
                nudgeMsg.content.push_back(TextContent{"Please provide the <done /> tag if you are finished, or continue using tools to solve the task."});
                auto nudgeNow = std::chrono::system_clock::now();
                nudgeMsg.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(nudgeNow.time_since_epoch()).count());
                nudgeTurn.messages.push_back(nudgeMsg);
                context.history.turns.push_back(nudgeTurn);
                if (context.config.persistHistory && journaler) journaler->appendTurn(nudgeTurn);
            } else {
                // --- State: ExecutingTool ---
                context.state.currentStatus = AgentStatus::ExecutingTool;

                // Track pending tool calls
                for (const auto& chunk : accumulatedToolChunks) {
                    context.state.pendingToolCalls.push_back(chunk.id);
                }

                auto toolStartMs = nowMs();
                executeTools(accumulatedToolChunks, onEvent);
                auto toolEndMs = nowMs();

                // Update the turn metrics with tool execution time
                // (The turn is already pushed to history, so update the last assistant turn in-place)
                auto& lastTurn = context.history.turns[context.history.turns.size() - 2]; // assistant turn is 2nd-to-last (tool result turn was just pushed by executeTools)
                lastTurn.metrics.timing.toolExecutionMs = toolEndMs - toolStartMs;

                // Also update the aggregate (only the tool timing delta)
                context.aggregateMetrics.timing.toolExecutionMs += (toolEndMs - toolStartMs);

                // Clear pending tool calls
                context.state.pendingToolCalls.clear();
            }

        } catch (const std::exception& e) {
            // --- State: Error ---
            context.state.currentStatus = AgentStatus::Error;
            context.state.fatalError = e.what();

            if (debugPrettyPrint) {
                std::cerr << "\033[1;31m[FATAL ERROR] " << e.what() << "\033[0m\n";
            }

            // Emit error as a StreamError event
            onEvent(StreamError{e.what(), 0});
            break;
        }
    }

    // --- Final state ---
    if (interrupted.load()) {
        context.state.currentStatus = AgentStatus::Cancelled;
    } else if (context.state.currentStatus != AgentStatus::Error) {
        context.state.currentStatus = AgentStatus::Idle;
    }

    running = false;

    if (debugPrettyPrint) {
        std::cout << "\n\033[1;36m--- Agent Finished ---\033[0m\n";
        std::cout << "Turns: " << turnCount << "\n";
        std::cout << "Cumulative Tokens: " << context.aggregateMetrics.tokens.total << "\n";
        std::cout << "  Prompt (billed): " << context.aggregateMetrics.tokens.prompt
                  << " (cached: " << context.aggregateMetrics.tokens.cacheRead << ")\n";
        std::cout << "  Completion: " << context.aggregateMetrics.tokens.completion
                  << " (reasoning: " << context.aggregateMetrics.tokens.reasoning << ")\n";
        std::cout << "Last Context Size: " << context.aggregateMetrics.tokens.contextSize << "\n";
        std::cout << "Total Cost: $" << context.aggregateMetrics.estimatedCostUsd << "\n";
        std::cout << "TTFT (first turn): " << context.aggregateMetrics.timing.firstTokenMs << "ms\n";
        std::cout << "Tool execution: " << context.aggregateMetrics.timing.toolExecutionMs << "ms\n";
    }
}

void Agent::executeTools(const std::vector<ToolCallChunk>& chunks, std::function<void(const StreamEvent&)> onEvent) {
    AgentTurn toolResultTurn;
    toolResultTurn.turnId = "tools-" + std::to_string(context.history.turns.size());

    for (const auto& chunk : chunks) {
        rapidjson::Document input;
        input.Parse(chunk.argsDelta.c_str());

        std::string resultStr;
        bool success = false;

        if (input.HasParseError()) {
            resultStr = "Invalid JSON arguments: " + chunk.argsDelta;
            success = false;
        } else {
            if (debugPrettyPrint) {
                std::cout << "\033[1;34m[Executing " << chunk.nameDelta << " with " << chunk.argsDelta << "]\033[0m\n";
            }

            ToolContext toolCtx{host, *this};
            auto result = toolRegistry.execute(chunk.nameDelta, input, toolCtx);

            if (result.success) {
                resultStr = result.data;
                success = true;
            } else {
                resultStr = "Error: " + result.error;
                success = false;
            }
        }

        if (debugPrettyPrint) {
            std::cout << "\033[1;32m[Result: " << resultStr.substr(0, 200) << (resultStr.size() > 200 ? "..." : "") << "]\033[0m\n";
        }

        Message msg;
        msg.role = Role::ToolResult;
        msg.content.push_back(ToolResultContent{chunk.id, resultStr, success});
        auto now = std::chrono::system_clock::now();
        msg.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        toolResultTurn.messages.push_back(msg);
    }

    context.history.turns.push_back(toolResultTurn);
    if (context.config.persistHistory && journaler) journaler->appendTurn(toolResultTurn);

    // Broadcast turn completion
    onEvent(AgentTurnCompleted{context.identity.id, toolResultTurn, context.aggregateMetrics});
}

void Agent::compactContext(std::function<void(const shared::StreamEvent&)> onEvent) {
    context.state.currentStatus = AgentStatus::Compacting;
    onEvent(AgentCompacting{context.identity.id});

    if (debugPrettyPrint) {
        std::cout << "\n\033[1;35m--- CONTEXT COMPACTION TRIGGERED (Size: " 
                  << context.aggregateMetrics.tokens.contextSize << ") ---\033[0m\n";
    }

    std::string compactionPrompt = PurposeLoader::loadCompactionPrompt();
    std::string summary = provider->generateSummary(context.history, compactionPrompt);

    uint32_t oldTokens = context.aggregateMetrics.tokens.contextSize;

    // Preserve Pinned Turns (0 and 1)
    std::vector<AgentTurn> pinned;
    if (context.history.turns.size() >= 2) {
        pinned.push_back(context.history.turns[0]);
        pinned.push_back(context.history.turns[1]);
    } else {
        pinned = context.history.turns;
    }

    context.history.turns.clear();
    for (auto& t : pinned) context.history.turns.push_back(t);

    // Create Synthetic Memory Turn
    AgentTurn summaryTurn;
    summaryTurn.turnId = "compaction-summary-" + std::to_string(nowMs());
    
    Message summaryMsg;
    summaryMsg.role = Role::User;
    summaryMsg.content.push_back(TextContent{"COMPACTION SUMMARY: " + summary});
    summaryMsg.timestamp = nowMs();
    
    summaryTurn.messages.push_back(summaryMsg);
    context.history.turns.push_back(summaryTurn);

    if (context.config.persistHistory && journaler) {
        journaler->appendTurn(summaryTurn);
    }

    uint32_t tokensSaved = (oldTokens > 1000) ? oldTokens - 1000 : 0; 
    onEvent(ContextCompacted{context.identity.id, tokensSaved});

    if (debugPrettyPrint) {
        std::cout << "\033[1;35m--- CONTEXT COMPACTED. SYNTHETIC MEMORY INJECTED. ---\033[0m\n";
    }
}

}
