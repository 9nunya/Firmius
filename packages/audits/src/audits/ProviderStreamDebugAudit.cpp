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
#include <set>

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

struct ScenarioExpectations {
    bool requireThinking = false;
    bool requireToolChunks = false;
    std::size_t minDistinctToolCalls = 0;
};

struct ScenarioStats {
    int eventCount = 0;
    int errorCount = 0;
    int textCount = 0;
    int thinkingCount = 0;
    int toolChunkCount = 0;
    std::set<std::string> toolCallIds;
};

std::vector<ToolDefinition> genericDebugTools() {
    return {
        {"list_files", "List files in a directory",
         R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"},
        {"read_file", "Read a file from disk",
         R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"},
        {"write_file", "Write file contents",
         R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})"},
        {"run_command", "Run a command",
         R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})"},
    };
}

std::vector<ToolDefinition> buildToolsForVariant(const std::string& variant) {
    if (variant == "thinking_long_tool_call") {
        return {
            {"write_strategy_packet",
             "Write a long strategy packet with executive summary, migration plan, and risk register",
             R"({"type":"object","properties":{"title":{"type":"string"},"executive_summary_markdown":{"type":"string"},"migration_plan_markdown":{"type":"string"},"risk_register_markdown":{"type":"string"},"open_questions":{"type":"array","items":{"type":"string"}},"decision_log":{"type":"array","items":{"type":"string"}}},"required":["title","executive_summary_markdown","migration_plan_markdown","risk_register_markdown","open_questions","decision_log"]})"},
        };
    }
    if (variant == "multi_turn_thinking_preparing") {
        return {
            {"revise_strategy_packet",
             "Rewrite an existing strategy packet after major new constraints arrive",
             R"({"type":"object","properties":{"title":{"type":"string"},"changed_constraints":{"type":"array","items":{"type":"string"}},"revised_executive_summary_markdown":{"type":"string"},"revised_migration_plan_markdown":{"type":"string"},"revised_risk_register_markdown":{"type":"string"},"validation_appendix_markdown":{"type":"string"}},"required":["title","changed_constraints","revised_executive_summary_markdown","revised_migration_plan_markdown","revised_risk_register_markdown","validation_appendix_markdown"]})"},
        };
    }
    if (variant == "parallel_tool_preparing") {
        return {
            {"write_exec_summary",
             "Write the executive summary document for leadership",
             R"({"type":"object","properties":{"title":{"type":"string"},"summary_markdown":{"type":"string"},"decision_points":{"type":"array","items":{"type":"string"}}},"required":["title","summary_markdown","decision_points"]})"},
            {"write_migration_plan",
             "Write the detailed migration plan document for engineering",
             R"({"type":"object","properties":{"title":{"type":"string"},"plan_markdown":{"type":"string"},"workstreams":{"type":"array","items":{"type":"string"}}},"required":["title","plan_markdown","workstreams"]})"},
            {"write_risk_register",
             "Write the risk register document for rollout and operations",
             R"({"type":"object","properties":{"title":{"type":"string"},"risk_markdown":{"type":"string"},"mitigations":{"type":"array","items":{"type":"string"}}},"required":["title","risk_markdown","mitigations"]})"},
        };
    }
    return genericDebugTools();
}

ScenarioExpectations expectationsForVariant(const std::string& variant) {
    if (variant == "thinking_long_tool_call") {
        ScenarioExpectations expectations;
        expectations.requireThinking = true;
        expectations.requireToolChunks = true;
        expectations.minDistinctToolCalls = 1;
        return expectations;
    }
    if (variant == "multi_turn_thinking_preparing") {
        ScenarioExpectations expectations;
        expectations.requireThinking = true;
        expectations.requireToolChunks = true;
        expectations.minDistinctToolCalls = 1;
        return expectations;
    }
    if (variant == "parallel_tool_preparing") {
        ScenarioExpectations expectations;
        expectations.requireThinking = true;
        expectations.requireToolChunks = true;
        expectations.minDistinctToolCalls = 3;
        return expectations;
    }
    return {};
}

std::optional<ModelVariant> findModelVariant(const std::vector<ModelInfo>& models,
                                             const std::string& modelId,
                                             const std::string& variantName) {
    if (variantName.empty()) {
        return std::nullopt;
    }
    for (const auto& model : models) {
        if (model.id != modelId) {
            continue;
        }
        for (const auto& variant : model.variants) {
            if (variant.variantName == variantName) {
                return variant;
            }
        }
        break;
    }
    return std::nullopt;
}
}

std::string ProviderStreamDebugAudit::getId() const { return "provider_stream_debug"; }

std::string ProviderStreamDebugAudit::getDescription() const {
    return "Debug: Log EVERY chunk from provider stream to STDOUT";
}

std::string ProviderStreamDebugAudit::resolveModelIdArg(
    const std::vector<std::string>& args) {
    for (size_t i = 1; i < args.size(); ++i) {
        if (!args[i].empty() && args[i].rfind("--", 0) != 0) {
            return args[i];
        }
    }
    return "";
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
        
    } else if (variant == "thinking_long_tool_call") {
        AgentTurn turn;
        Message msg;
        msg.role = Role::User;
        msg.content.push_back(TextContent{
            "You are writing a strategy packet for a high-risk platform change. "
            "Think carefully first, because the problem is intentionally hard: "
            "a company must migrate from session cookies plus background API "
            "keys to short-lived scoped service tokens, without forcing mobile "
            "logout, while preserving auditability, supporting multi-region "
            "failover, and surviving a staged migration where old and new auth "
            "systems coexist for 30 days. There are conflicting goals around "
            "latency, blast radius, token revocation, support operations, and "
            "customer-visible incident handling. Then call exactly one tool, "
            "`write_strategy_packet`, before any prose. Make the tool payload "
            "long: write substantial markdown for an executive summary, a "
            "detailed migration plan, and a risk register, each rich enough to "
            "read like the draft of a real internal document."});
        turn.messages.push_back(msg);
        history.turns.push_back(turn);

    } else if (variant == "multi_turn_thinking_preparing") {
        AgentTurn turn1;
        Message userMsg;
        userMsg.role = Role::User;
        userMsg.content.push_back(TextContent{
            "We need a delivery plan for migrating our authentication service."});
        turn1.messages.push_back(userMsg);
        history.turns.push_back(turn1);

        AgentTurn turn2;
        Message assistantTool;
        assistantTool.role = Role::Assistant;
        ToolCallContent toolCall;
        toolCall.id = "plan_call_1";
        toolCall.name = "revise_strategy_packet";
        toolCall.args =
            R"({"title":"Auth migration packet v1","changed_constraints":["maintain uptime during rollout"],"revised_executive_summary_markdown":"Initial packet focusing on service-token migration and compatibility windows.","revised_migration_plan_markdown":"Phase 1 inventory, Phase 2 compatibility layer, Phase 3 shadow validation.","revised_risk_register_markdown":"Primary risks: stale sessions, token propagation lag, support confusion.","validation_appendix_markdown":"Baseline metrics and smoke checks captured."})";
        assistantTool.content.push_back(toolCall);
        turn2.messages.push_back(assistantTool);
        history.turns.push_back(turn2);

        AgentTurn turn3;
        Message toolResult;
        toolResult.role = Role::ToolResult;
        ToolResultContent result;
        result.toolCallId = "plan_call_1";
        result.result =
            R"({"status":"stored","version":"v1","notes":"Initial auth migration plan recorded."})";
        toolResult.content.push_back(result);
        turn3.messages.push_back(toolResult);
        history.turns.push_back(turn3);

        AgentTurn turn4;
        Message followupUser;
        followupUser.role = Role::User;
        followupUser.content.push_back(TextContent{
            "A second wave of constraints arrived after leadership review. "
            "Think again before acting. The migration packet must now handle "
            "multi-region failover, backwards-compatible parsing of legacy "
            "sessions for 30 days, mobile client rollout with stale app "
            "versions, emergency token revocation under partial outages, legal "
            "requirements for audit retention, and a customer-support playbook "
            "for account lockouts. Call `revise_strategy_packet` before any "
            "prose. Make the new tool payload substantially larger than the "
            "earlier packet and rewrite it like a serious revision, not a short "
            "patch."});
        turn4.messages.push_back(followupUser);
        history.turns.push_back(turn4);

    } else if (variant == "parallel_tool_preparing") {
        AgentTurn turn;
        Message msg;
        msg.role = Role::User;
        msg.content.push_back(TextContent{
            "This is a heavy writing task with three independent deliverables "
            "for the same risky auth migration: a leadership executive summary, "
            "an engineering migration plan, and an operations risk register. "
            "Think first because the tradeoffs are real: phased coexistence of "
            "old and new auth, mobile rollout lag, regional failover, token "
            "revocation semantics, customer support burden, and auditability. "
            "Then prepare multiple tool calls before any prose. Call "
            "`write_exec_summary`, `write_migration_plan`, and "
            "`write_risk_register` in parallel. Each tool payload should carry "
            "substantial markdown that reads like the start of a real document. "
            "Do not merge them into one tool call and do not answer normally "
            "before those tools are prepared."});
        turn.messages.push_back(msg);
        history.turns.push_back(turn);
        
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
        std::cerr << "Usage: firmius_audit --audit provider_stream_debug <provider_id> [model_id] [--history-variant=<variant>] [--variant=<model-variant>] [--raw-sse-log=<path>] [--raw-sse-stdout]" << std::endl;
        std::cerr << "       firmius_audit --audit provider_stream_debug <provider_id> [model_id] --thread-id=<threadId> [--thread-agent=<agentId>] [--variant=<model-variant>]" << std::endl;
        std::cerr << "       firmius_audit --audit provider_stream_debug <provider_id> [model_id] [--variant=<model-variant>] --tool-preparing-suite" << std::endl;
        std::cerr << "Example: firmius_audit --audit provider_stream_debug antigravity claude-opus-4-6-thinking --variant=max --history-variant=thinking_long_tool_call" << std::endl;
        std::cerr << std::endl;
        std::cerr << "History variants for testing edge cases:" << std::endl;
        std::cerr << "  --history-variant=normal_agentic       Standard conversation" << std::endl;
        std::cerr << "  --history-variant=agentic_tool_errors  Tool errors in chat" << std::endl;
        std::cerr << "  --history-variant=multiple_tool_results Multiple tool results" << std::endl;
        std::cerr << "  --history-variant=tool_then_error      Tool result then error" << std::endl;
        std::cerr << "  --history-variant=error_then_tool      Error then tool result" << std::endl;
        std::cerr << "  --history-variant=mixed_agentic        Complex mixed scenario" << std::endl;
        std::cerr << "  --history-variant=thinking_long_tool_call" << std::endl;
        std::cerr << "  --history-variant=multi_turn_thinking_preparing" << std::endl;
        std::cerr << "  --history-variant=parallel_tool_preparing" << std::endl;
        std::cerr << "  --tool-preparing-suite                 Run all three preparing/thinking scenarios and fail if expected events are missing" << std::endl;
        std::cerr << "  --raw-sse-log=/tmp/provider_sse.log    Capture raw SSE chunks and lines before parsing" << std::endl;
        std::cerr << "  --raw-sse-stdout                      Mirror raw SSE to stdout during the run" << std::endl;
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
    std::string modelId = resolveModelIdArg(args);
    
    // Parse history variant and thread flags
    std::string historyVariant = "default";
    std::string threadId;
    std::string threadAgentId;
    std::string modelVariantName;
    std::string rawSseLogPath;
    bool toolPreparingSuite = false;
    bool rawSseStdout = false;
    for (const auto& arg : args) {
        if (arg.find("--history-variant=") == 0) {
            historyVariant = arg.substr(18);
        } else if (arg.find("--thread-id=") == 0) {
            threadId = arg.substr(12);
        } else if (arg.find("--thread-agent=") == 0) {
            threadAgentId = arg.substr(15);
        } else if (arg.find("--variant=") == 0) {
            modelVariantName = arg.substr(10);
        } else if (arg.find("--model-variant=") == 0) {
            modelVariantName = arg.substr(16);
        } else if (arg == "--tool-preparing-suite") {
            toolPreparingSuite = true;
        } else if (arg.find("--raw-sse-log=") == 0) {
            rawSseLogPath = arg.substr(14);
        } else if (arg == "--raw-sse-stdout") {
            rawSseStdout = true;
        } else if (arg == "--raw-sse") {
            rawSseLogPath = "/tmp/firmius_provider_raw_sse.log";
        }
    }

    if (toolPreparingSuite && !threadId.empty()) {
        std::cerr << "--tool-preparing-suite cannot be combined with --thread-id" << std::endl;
        result.exitCode = 1;
        result.passed = false;
        return result;
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

    ProviderOptions baseOpts;
    baseOpts.modelId = modelId;
    baseOpts.temperature = 0.7f;
    if (!modelVariantName.empty()) {
        auto variant = findModelVariant(models, modelId, modelVariantName);
        if (!variant.has_value()) {
            std::cerr << "Model variant not found: " << modelVariantName
                      << " for model " << modelId << std::endl;
            result.exitCode = 1;
            result.passed = false;
            return result;
        }
        baseOpts.modelVariantJson = variant->extraMetadataJson;
    }

    std::cout << "Model: " << modelId << std::endl;
    if (!modelVariantName.empty()) {
        std::cout << "Model Variant: " << modelVariantName << std::endl;
    }
    if (!rawSseLogPath.empty()) {
        std::cout << "Raw SSE Log: " << rawSseLogPath << std::endl;
    }
    if (rawSseStdout) {
        std::cout << "Raw SSE Stdout: enabled" << std::endl;
    }
    if (!threadId.empty()) {
        std::cout << "Thread Id: " << threadId << std::endl;
        if (!threadAgentId.empty()) {
            std::cout << "Thread Agent: " << threadAgentId << std::endl;
        }
    } else if (toolPreparingSuite) {
        std::cout << "Scenario Suite: tool_preparing_suite" << std::endl;
    } else {
        std::cout << "History Variant: " << historyVariant << std::endl;
    }
    std::cout << "====================================" << std::endl;
    std::cout << std::endl;

    if (!rawSseLogPath.empty()) {
        std::ofstream truncate(rawSseLogPath, std::ios::trunc);
        truncate.close();
        setenv("FIRMIUS_ANTIGRAVITY_RAW_SSE_LOG", rawSseLogPath.c_str(), 1);
        setenv("FIRMIUS_CODEX_RAW_SSE_LOG", rawSseLogPath.c_str(), 1);
    } else {
        unsetenv("FIRMIUS_ANTIGRAVITY_RAW_SSE_LOG");
        unsetenv("FIRMIUS_CODEX_RAW_SSE_LOG");
    }
    if (rawSseStdout) {
        setenv("FIRMIUS_ANTIGRAVITY_RAW_SSE_STDOUT", "1", 1);
        setenv("FIRMIUS_CODEX_RAW_SSE_STDOUT", "1", 1);
    } else {
        unsetenv("FIRMIUS_ANTIGRAVITY_RAW_SSE_STDOUT");
        unsetenv("FIRMIUS_CODEX_RAW_SSE_STDOUT");
    }

    auto runScenario = [&](const std::string& scenarioName,
                           const AgentHistory& history,
                           const std::vector<ToolDefinition>& tools,
                           const ScenarioExpectations& expectations) {
        ProviderOptions opts = baseOpts;
        opts.tools = tools;

        std::cout << "--- Scenario: " << scenarioName << " ---" << std::endl;
        std::cout << "Tool count: " << tools.size() << std::endl;
        for (const auto& tool : tools) {
            std::cout << "  tool: " << tool.name << std::endl;
        }
        if (!rawSseLogPath.empty()) {
            std::ofstream raw(rawSseLogPath, std::ios::app);
            if (raw.is_open()) {
                raw << "\n=== SCENARIO " << scenarioName << " ===\n";
            }
        }
        std::cout << "--- Stream Events (timestamp | type | data) ---" << std::endl;

        ScenarioStats stats;
        provider->stream(history, opts, [&](const StreamEvent& ev) {
            std::string ts = getTimestamp();
            stats.eventCount++;

            if (auto* txt = std::get_if<TextChunk>(&ev)) {
                stats.textCount++;
                std::cout << "[" << ts << "] [TEXT] \"" << escapeString(txt->delta) << "\"" << std::endl;
            } else if (auto* thk = std::get_if<ThinkingChunk>(&ev)) {
                stats.thinkingCount++;
                std::cout << "[" << ts << "] [THINKING] delta=\"" << escapeString(thk->delta)
                          << "\" signature=\"" << escapeString(thk->signature) << "\"" << std::endl;
            } else if (auto* tcc = std::get_if<ToolCallChunk>(&ev)) {
                stats.toolChunkCount++;
                std::string toolKey = !tcc->id.empty()
                                          ? tcc->id
                                          : ("index:" + std::to_string(tcc->index));
                stats.toolCallIds.insert(toolKey);
                std::cout << "[" << ts << "] [TOOL_CHUNK] index=" << tcc->index
                          << " id=" << tcc->id
                          << " name=" << tcc->nameDelta
                          << " args=" << escapeString(tcc->argsDelta) << std::endl;
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
                stats.errorCount++;
                std::cout << "[" << ts << "] [STREAM_ERROR] httpStatus=" << err->httpStatus
                          << " account=" << err->accountLocator
                          << " message=\"" << err->message << "\"" << std::endl;
            } else if (auto* retry = std::get_if<StreamRetrying>(&ev)) {
                std::cout << "[" << ts << "] [STREAM_RETRYING] attempt=" << retry->attempt
                          << " of " << retry->maxAttempts
                          << " delay_ms=" << retry->delayMs
                          << " reason=\"" << retry->reason << "\"" << std::endl;
            } else if (auto* switched = std::get_if<StreamAccountSwitched>(&ev)) {
                std::cout << "[" << ts << "] [STREAM_ACCOUNT_SWITCHED] account="
                          << switched->accountLocator << std::endl;
            } else {
                std::cout << "[" << ts << "] [UNKNOWN_EVENT] variant_index="
                          << ev.index() << std::endl;
            }
            std::cout << std::flush;
        });

        bool passedScenario = stats.errorCount == 0;
        if (expectations.requireThinking && stats.thinkingCount == 0) {
            std::cout << "[ASSERT] Missing thinking events" << std::endl;
            passedScenario = false;
        }
        if (expectations.requireToolChunks && stats.toolChunkCount == 0) {
            std::cout << "[ASSERT] Missing tool preparing chunks" << std::endl;
            passedScenario = false;
        }
        if (stats.toolCallIds.size() < expectations.minDistinctToolCalls) {
            std::cout << "[ASSERT] Expected at least "
                      << expectations.minDistinctToolCalls
                      << " distinct tool calls but saw "
                      << stats.toolCallIds.size() << std::endl;
            passedScenario = false;
        }

        std::cout << std::endl;
        std::cout << "=== Scenario Summary ===" << std::endl;
        std::cout << "Events: " << stats.eventCount << std::endl;
        std::cout << "Thinking events: " << stats.thinkingCount << std::endl;
        std::cout << "Tool chunks: " << stats.toolChunkCount << std::endl;
        std::cout << "Distinct tool calls: " << stats.toolCallIds.size() << std::endl;
        std::cout << "Errors: " << stats.errorCount << std::endl;
        std::cout << "Scenario result: " << (passedScenario ? "PASS" : "FAIL") << std::endl;
        std::cout << std::endl;
        return passedScenario;
    };

    bool allPassed = true;
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
        AgentHistory history = tm.loadAgentHistory(threadId, threadAgentId);
        allPassed = runScenario("thread:" + threadId, history, {}, {});
    } else if (toolPreparingSuite) {
        const std::vector<std::string> scenarios = {
            "thinking_long_tool_call",
            "multi_turn_thinking_preparing",
            "parallel_tool_preparing",
        };
        for (const auto& scenario : scenarios) {
            const bool scenarioPassed =
                runScenario(scenario, buildHistoryVariant(scenario),
                            buildToolsForVariant(scenario),
                            expectationsForVariant(scenario));
            allPassed = allPassed && scenarioPassed;
        }
    } else {
        allPassed = runScenario(historyVariant, buildHistoryVariant(historyVariant),
                                buildToolsForVariant(historyVariant),
                                expectationsForVariant(historyVariant));
    }

    unsetenv("FIRMIUS_ANTIGRAVITY_RAW_SSE_LOG");
    unsetenv("FIRMIUS_ANTIGRAVITY_RAW_SSE_STDOUT");
    unsetenv("FIRMIUS_CODEX_RAW_SSE_LOG");
    unsetenv("FIRMIUS_CODEX_RAW_SSE_STDOUT");
    result.exitCode = allPassed ? 0 : 1;
    result.passed = allPassed;
    return result;
}

} // namespace firmius::audits
