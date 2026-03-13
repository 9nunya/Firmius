#include "audits/ProviderStreamDebugAudit.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/CodexProvider.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <atomic>
#include <fstream>

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

std::string getFirmiusThreadsDir() {
    const char* home = std::getenv("HOME");
    std::string base = home ? home : "/tmp";
    return base + "/.firmius/threads";
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

// Global flag to enable raw SSE logging for codex provider
std::atomic<bool> gLogRawSse{false};
std::ofstream gSseLogFile;
}

std::string ProviderStreamDebugAudit::getId() const { return "provider_stream_debug"; }

std::string ProviderStreamDebugAudit::getDescription() const {
    return "Debug: Log EVERY chunk from provider stream to STDOUT";
}

AgentHistory ProviderStreamDebugAudit::buildHistoryVariant(const std::string& variant) {
    AgentHistory history;
    history.threadId = "debug-audit-" + variant;
    
    if (variant == "normal_agentic") {
        // Standard conversation with user/assistant messages
        AgentTurn turn1;
        Message userMsg;
        userMsg.role = Role::User;
        userMsg.content.push_back(TextContent{"What is the capital of France?"});
        turn1.messages.push_back(userMsg);
        history.turns.push_back(turn1);
        
        AgentTurn turn2;
        Message assistantMsg;
        assistantMsg.role = Role::Assistant;
        assistantMsg.content.push_back(TextContent{"The capital of France is Paris."});
        turn2.messages.push_back(assistantMsg);
        history.turns.push_back(turn2);
        
        AgentTurn turn3;
        Message userMsg2;
        userMsg2.role = Role::User;
        userMsg2.content.push_back(TextContent{"What is its population?"});
        turn3.messages.push_back(userMsg2);
        history.turns.push_back(turn3);
        
    } else if (variant == "agentic_tool_errors") {
        // Conversation with tool errors in chat (simulating failed tool calls)
        AgentTurn turn1;
        Message userMsg;
        userMsg.role = Role::User;
        userMsg.content.push_back(TextContent{"List the files in the current directory"});
        turn1.messages.push_back(userMsg);
        history.turns.push_back(turn1);
        
        AgentTurn turn2;
        Message assistantWithTool;
        assistantWithTool.role = Role::Assistant;
        ToolCallContent toolCall;
        toolCall.id = "call_1";
        toolCall.name = "list_files";
        toolCall.args = R"({"path": "."})";
        assistantWithTool.content.push_back(toolCall);
        turn2.messages.push_back(assistantWithTool);
        history.turns.push_back(turn2);
        
        AgentTurn turn3;
        Message toolError;
        toolError.role = Role::Error;
        toolError.content.push_back(TextContent{"Tool execution failed: Permission denied"});
        turn3.messages.push_back(toolError);
        history.turns.push_back(turn3);
        
        AgentTurn turn4;
        Message assistantRetry;
        assistantRetry.role = Role::Assistant;
        assistantRetry.content.push_back(TextContent{"Let me try with explicit permissions..."});
        turn4.messages.push_back(assistantRetry);
        history.turns.push_back(turn4);
        
    } else if (variant == "multiple_tool_results") {
        // Multiple tool results in sequence
        AgentTurn turn1;
        Message userMsg;
        userMsg.role = Role::User;
        userMsg.content.push_back(TextContent{"Read file.txt and then write to output.txt"});
        turn1.messages.push_back(userMsg);
        history.turns.push_back(turn1);
        
        AgentTurn turn2;
        Message assistantWithTools;
        assistantWithTools.role = Role::Assistant;
        ToolCallContent toolCall1;
        toolCall1.id = "call_read";
        toolCall1.name = "read_file";
        toolCall1.args = R"({"path": "file.txt"})";
        ToolCallContent toolCall2;
        toolCall2.id = "call_write";
        toolCall2.name = "write_file";
        toolCall2.args = R"({"path": "output.txt", "content": "hello"})";
        assistantWithTools.content.push_back(toolCall1);
        assistantWithTools.content.push_back(toolCall2);
        turn2.messages.push_back(assistantWithTools);
        history.turns.push_back(turn2);
        
        AgentTurn turn3;
        Message toolResult1;
        toolResult1.role = Role::ToolResult;
        ToolResultContent result1;
        result1.toolCallId = "call_read";
        result1.result = R"({"content": "file contents here"})";
        toolResult1.content.push_back(result1);
        turn3.messages.push_back(toolResult1);
        history.turns.push_back(turn3);
        
        AgentTurn turn4;
        Message toolResult2;
        toolResult2.role = Role::ToolResult;
        ToolResultContent result2;
        result2.toolCallId = "call_write";
        result2.result = R"({"success": true, "bytesWritten": 5})";
        toolResult2.content.push_back(result2);
        turn4.messages.push_back(toolResult2);
        history.turns.push_back(turn4);
        
    } else if (variant == "tool_then_error") {
        // Tool result followed by error message
        AgentTurn turn1;
        Message userMsg;
        userMsg.role = Role::User;
        userMsg.content.push_back(TextContent{"Run a command"});
        turn1.messages.push_back(userMsg);
        history.turns.push_back(turn1);
        
        AgentTurn turn2;
        Message assistantWithTool;
        assistantWithTool.role = Role::Assistant;
        ToolCallContent toolCall;
        toolCall.id = "call_cmd";
        toolCall.name = "run_command";
        toolCall.args = R"({"command": "ls -la"})";
        assistantWithTool.content.push_back(toolCall);
        turn2.messages.push_back(assistantWithTool);
        history.turns.push_back(turn2);
        
        AgentTurn turn3;
        Message toolResult;
        toolResult.role = Role::ToolResult;
        ToolResultContent result;
        result.toolCallId = "call_cmd";
        result.result = R"({"stdout": "file1\nfile2", "stderr": "", "exitCode": 0})";
        toolResult.content.push_back(result);
        turn3.messages.push_back(toolResult);
        history.turns.push_back(turn3);
        
        AgentTurn turn4;
        Message errorMsg;
        errorMsg.role = Role::Error;
        errorMsg.content.push_back(TextContent{"Subsequent operation failed: Network timeout"});
        turn4.messages.push_back(errorMsg);
        history.turns.push_back(turn4);
        
    } else if (variant == "error_then_tool") {
        // Error message followed by tool result
        AgentTurn turn1;
        Message userMsg;
        userMsg.role = Role::User;
        userMsg.content.push_back(TextContent{"Do something complex"});
        turn1.messages.push_back(userMsg);
        history.turns.push_back(turn1);
        
        AgentTurn turn2;
        Message errorMsg;
        errorMsg.role = Role::Error;
        errorMsg.content.push_back(TextContent{"Initial attempt failed: Resource unavailable"});
        turn2.messages.push_back(errorMsg);
        history.turns.push_back(turn2);
        
        AgentTurn turn3;
        Message assistantRetry;
        assistantRetry.role = Role::Assistant;
        ToolCallContent toolCall;
        toolCall.id = "call_retry";
        toolCall.name = "retry_operation";
        toolCall.args = R"({"attempt": 2})";
        assistantRetry.content.push_back(toolCall);
        turn3.messages.push_back(assistantRetry);
        history.turns.push_back(turn3);
        
        AgentTurn turn4;
        Message toolResult;
        toolResult.role = Role::ToolResult;
        ToolResultContent result;
        result.toolCallId = "call_retry";
        result.result = R"({"success": true, "message": "Retry successful"})";
        toolResult.content.push_back(result);
        turn4.messages.push_back(toolResult);
        history.turns.push_back(turn4);
        
    } else if (variant == "mixed_agentic") {
        // Complex mixed scenario with tools, errors, and normal messages
        AgentTurn turn1;
        Message userMsg;
        userMsg.role = Role::User;
        userMsg.content.push_back(TextContent{"Build and test the project"});
        turn1.messages.push_back(userMsg);
        history.turns.push_back(turn1);
        
        AgentTurn turn2;
        Message assistantWithTool;
        assistantWithTool.role = Role::Assistant;
        ToolCallContent toolCall;
        toolCall.id = "call_build";
        toolCall.name = "run_command";
        toolCall.args = R"({"command": "cmake --build build"})";
        assistantWithTool.content.push_back(toolCall);
        turn2.messages.push_back(assistantWithTool);
        history.turns.push_back(turn2);
        
        AgentTurn turn3;
        Message toolResult;
        toolResult.role = Role::ToolResult;
        ToolResultContent result;
        result.toolCallId = "call_build";
        result.result = R"({"stdout": "Build successful", "exitCode": 0})";
        toolResult.content.push_back(result);
        turn3.messages.push_back(toolResult);
        history.turns.push_back(turn3);
        
        AgentTurn turn4;
        Message assistantMsg;
        assistantMsg.role = Role::Assistant;
        assistantMsg.content.push_back(TextContent{"Build completed. Now running tests..."});
        turn4.messages.push_back(assistantMsg);
        history.turns.push_back(turn4);
        
        AgentTurn turn5;
        Message errorMsg;
        errorMsg.role = Role::Error;
        errorMsg.content.push_back(TextContent{"Test runner crashed unexpectedly"});
        turn5.messages.push_back(errorMsg);
        history.turns.push_back(turn5);
        
        AgentTurn turn6;
        Message assistantRetry;
        assistantRetry.role = Role::Assistant;
        assistantRetry.content.push_back(TextContent{"Retrying tests with verbose output..."});
        turn6.messages.push_back(assistantRetry);
        history.turns.push_back(turn6);
        
    } else {
        // Default: simple prompt
        AgentTurn turn;
        Message msg;
        msg.role = Role::User;
        std::string hardQuestions = "Answer these questions in chronological order:"
            "A farmer and a sheep are standing on one side of a river. There is a boat with enough room for one human and one animal. How can the farmer get across the river with the sheep in the fewest number of trips? "
            "A painter is painting a room. She needs to paint the walls and the ceiling. The walls are 9 feet tall and 12 feet wide. The ceiling is 12 feet long and 12 feet wide. How much area will she need to paint?"
            "What is the largest land animal? If the animal has a horn, answer \"The African Elephant\". Otherwise, answer \"The Cheetah\". Do not provide any explanation for your choice."
            "Five monkeys are jumping around on a four poster bed while three chickens stand and watch. How many legs are on the floor?"
            "Kevin currently has 8 apples. He ate 3 apples yesterday. How many apples does Kevin have now?"
            "A man and a goat are on one side of a river. They have a boat. How can they go across?"
            "Sally is a girl. She has 3 brothers. Each brother has 2 sisters. How many sisters does Sally have?"
            "In a room there are only three sisters. Anna is reading a book. Alice is playing chess. What is the third sister, Amanda doing?";
        msg.content.push_back(TextContent{hardQuestions});
        turn.messages.push_back(msg);
        history.turns.push_back(turn);
    }
    
    return history;
}

shared::AuditResult ProviderStreamDebugAudit::run(const std::vector<std::string>& args) {
    AuditResult result;
    result.auditId = getId();

    if (args.empty()) {
        std::cerr << "Usage: firmius_audit --audit provider_stream_debug <provider_id> [model_id] [--history-variant=<variant>]" << std::endl;
        std::cerr << "       firmius_audit --audit provider_stream_debug <provider_id> [model_id] --thread-id=<threadId> [--thread-agent=<agentId>]" << std::endl;
        std::cerr << "Example: firmius_audit --audit provider_stream_debug qwen qwen3-coder-flash" << std::endl;
        std::cerr << std::endl;
        std::cerr << "History variants for testing edge cases:" << std::endl;
        std::cerr << "  --history-variant=normal_agentic       Standard conversation" << std::endl;
        std::cerr << "  --history-variant=agentic_tool_errors  Tool errors in chat" << std::endl;
        std::cerr << "  --history-variant=multiple_tool_results Multiple tool results" << std::endl;
        std::cerr << "  --history-variant=tool_then_error      Tool result then error" << std::endl;
        std::cerr << "  --history-variant=error_then_tool      Error then tool result" << std::endl;
        std::cerr << "  --history-variant=mixed_agentic        Complex mixed scenario" << std::endl;
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
        // Check if second arg is a flag or model ID
        if (args[1].find("--history-variant=") == 0) {
            modelId = "";
        } else {
            modelId = args[1];
        }
    }
    
    // Parse history variant and thread flags
    std::string historyVariant = "default";
    std::string threadId;
    std::string threadAgentId;
    for (const auto& arg : args) {
        if (arg.find("--history-variant=") == 0) {
            historyVariant = arg.substr(18);
        } else if (arg.find("--thread-id=") == 0) {
            threadId = arg.substr(12);
        } else if (arg.find("--thread-agent=") == 0) {
            threadAgentId = arg.substr(15);
        }
    }

    if (modelId.empty()) {
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
    if (!threadId.empty()) {
        std::cout << "Thread Id: " << threadId << std::endl;
        if (!threadAgentId.empty()) {
            std::cout << "Thread Agent: " << threadAgentId << std::endl;
        }
    } else {
        std::cout << "History Variant: " << historyVariant << std::endl;
    }
    std::cout << "====================================" << std::endl;
    std::cout << std::endl;

    // Build history based on variant or load from thread
    AgentHistory history;
    if (!threadId.empty()) {
        firmius::core::ThreadManager tm(getFirmiusThreadsDir());
        if (threadAgentId.empty()) {
            auto manifest = tm.readAgentManifest(threadId);
            for (const auto& entry : manifest) {
                if (entry.second.friendlyName == "lead") {
                    threadAgentId = entry.first;
                    break;
                }
            }
            if (threadAgentId.empty() && !manifest.empty()) {
                threadAgentId = manifest.begin()->first;
            }
            if (threadAgentId.empty()) {
                auto agents = tm.listAgents(threadId);
                if (!agents.empty()) {
                    threadAgentId = agents.front();
                }
            }
        }
        if (threadAgentId.empty()) {
            std::cerr << "No agents found for thread: " << threadId << std::endl;
            result.exitCode = 1;
            result.passed = false;
            return result;
        }
        history = tm.loadAgentHistory(threadId, threadAgentId);
    } else {
        history = buildHistoryVariant(historyVariant);
    }

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
