#include "agents/Agent.hpp"
#include "agents/PurposeLoader.hpp"
#include "utils/FSUtil.hpp"
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

namespace firmius::core {

using namespace firmius::shared;

/**
 * @file Agent.cpp
 * @brief Implementation of the Agent Engine loop and tool execution logic.
 */

Agent::Agent(AgentContext ctx, firmius::provider::IProvider& prov, shared::IHost& h, ToolRegistry& reg)
    : context(std::move(ctx)), provider(prov), host(h), toolRegistry(reg) {
    debugPrettyPrint = (std::getenv("FIRMIUS_PRETTY_PRINT") != nullptr);
}

void Agent::reset() {
    context.history.turns.clear();
}

std::string Agent::resolvePath(const std::string& inputPath) const {
    return shared::FSUtil::resolvePath(inputPath, context.environment.cwd);
}

void Agent::run(const std::string& task, std::function<void(const shared::StreamEvent&)> onEvent) {
    // 1. Bootstrap System Message if history is empty
    if (context.history.turns.empty()) {
        auto toolDefs = toolRegistry.getAvailableToolDefinitions(context.permissions);
        Persona persona = PurposeLoader::load("coder");
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
    }

    // 3. Autonomous Loop
    bool taskFinished = false;
    int maxTurns = 200;
    int turnCount = 0;

    while (!taskFinished && turnCount < maxTurns) {
        turnCount++;
        firmius::provider::ProviderOptions opts;
        opts.modelId = "zai-org/glm-4.7:thinking"; 
        opts.tools = toolRegistry.getAvailableToolDefinitions(context.permissions);

        std::vector<ToolCallChunk> accumulatedToolChunks;
        std::string fullResponse;
        std::string fullThinking;
        
        if (debugPrettyPrint) std::cout << "\n\033[1;36m--- Agent Thinking (Turn " << turnCount << ") ---\033[0m\n";

        provider.stream(context.history, opts, [&](const StreamEvent& ev) {
            onEvent(ev);
            
            if (auto* txt = std::get_if<TextChunk>(&ev)) {
                fullResponse += txt->delta;
                if (debugPrettyPrint) std::cout << txt->delta << std::flush;
            } else if (auto* thk = std::get_if<ThinkingChunk>(&ev)) {
                fullThinking += thk->delta;
                if (debugPrettyPrint) std::cout << "\033[3;37m" << thk->delta << "\033[0m" << std::flush;
            } else if (auto* tcc = std::get_if<ToolCallChunk>(&ev)) {
                bool found = false;
                for (auto& existing : accumulatedToolChunks) {
                    if (existing.index == tcc->index) {
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
                    accumulatedToolChunks.push_back(newChunk);
                }
                
                if (debugPrettyPrint && !tcc->nameDelta.empty()) {
                    std::cout << "\n\033[1;33m[Tool Call: " << tcc->nameDelta << "]\033[0m" << std::flush;
                }
            }
        });

        if (debugPrettyPrint) std::cout << "\n";

        AgentTurn assistantTurn;
        assistantTurn.turnId = "assistant-" + std::to_string(context.history.turns.size());
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

        // Check for termination
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
        } else {
            executeTools(accumulatedToolChunks, onEvent);
        }
    }
}

void Agent::executeTools(const std::vector<ToolCallChunk>& chunks, std::function<void(const StreamEvent&)>) {
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
                rapidjson::StringBuffer sb;
                rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
                result.data.Accept(writer);
                resultStr = sb.GetString();
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
}

}
