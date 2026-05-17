#include "audits/TokenWasteAudit.hpp"
#include "ConfigLoader.hpp"
#include "EnvLoader.hpp"
#include "Message.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"
#include "providers/ProviderRegistry.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

namespace {

constexpr auto kAgentSpawnTimeout = std::chrono::seconds(30);

struct ToolResultRecord {
    std::string toolName;
    size_t resultBytes = 0;
    bool success = false;
    uint32_t promptDelta = 0;  // actual provider-reported tokens added this turn
    uint32_t ctxAfter = 0;    // total context tokens after this result was processed
};

struct AuditState {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool hadError = false;
    std::string errorMessage;
    std::vector<ToolResultRecord> toolResults;
    uint32_t totalInputTokens = 0;
    uint32_t totalOutputTokens = 0;
    uint32_t totalTokens = 0;
    std::string lastToolName;
    uint32_t prevMetricsPrompt = 0;  // prompt tokens from last AgentMetricsStreamed
};

struct ParsedArgs {
    std::string providerId = "gitlawb";
    std::string modelId = "mimo-v2.5-pro";
    std::string modelVariant;
    std::string persona = "coder";
    int timeoutSeconds = 600;
};

ParsedArgs parseArgs(const std::vector<std::string>& args) {
    ParsedArgs parsed;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: firmius_audit --audit token_waste [options]\n"
                      << "  --provider <id>          Provider id (default: gitlawb)\n"
                      << "  --model <id>             Model id (default: mimo-v2.5-pro)\n"
                      << "  --variant <name>         Model variant (default: none)\n"
                      << "  --persona <name>         Persona to run (default: coder)\n"
                      << "  --timeout-seconds <n>    Overall timeout (default: 600)\n";
            throw std::runtime_error("help");
        }
        auto consume = [&](const std::string& longFlag) -> std::optional<std::string> {
            const std::string pfx = longFlag + "=";
            if (arg == longFlag && i + 1 < args.size()) { return args[++i]; }
            if (arg.rfind(pfx, 0) == 0) { return arg.substr(pfx.size()); }
            return std::nullopt;
        };
        if (auto v = consume("--provider")) { parsed.providerId = *v; continue; }
        if (auto v = consume("--model"))    { parsed.modelId    = *v; continue; }
        if (auto v = consume("--variant"))  { parsed.modelVariant = *v; continue; }
        if (auto v = consume("--persona"))  { parsed.persona    = *v; continue; }
        if (auto v = consume("--timeout-seconds")) { parsed.timeoutSeconds = std::stoi(*v); continue; }
        throw std::runtime_error("Unknown argument: " + arg);
    }
    return parsed;
}

std::string buildScenarioPrompt() {
    return R"(You are participating in a token-waste audit. Exercise each tool to produce the largest possible output for that call. Complete ALL tasks in order without skipping any.

SCENARIO 1 (grep_budget):
Use Files action=Grep, path='.', pattern='void', context_after=2. Report the result count and whether budget_hit was true.

SCENARIO 2 (glob_budget):
Use Files action=Glob, path='.', glob='**/*'. Report the result count and whether budget_hit was true.

SCENARIO 3 (file_read_no_range):
Use Files action=Read on 'compile_commands.json' with NO start_line or end_line. Report lines_read and whether truncated was true.

SCENARIO 4 (process_execute_stdout):
Use Process action=Execute with command: find /usr -maxdepth 3 -type f 2>/dev/null
Report the number of stdout lines.

SCENARIO 5 (editwrite_triple_echo):
Use EditWrite to create /tmp/twa_large.hpp containing exactly 200 lines of C++ code (header guard, namespace, 196 unique inline int functions). Note the size of the response including diff_preview, operations[0].old_lines, and operations[0].new_lines.

SCENARIO 6 (artifact_update_echo):
First, use Artifacts action=Write to create artifact 'twa-payload' with at least 500 words of lorem ipsum content.
Then immediately use Artifacts action=Write again with the same name and slightly different content.
Report the sizes of both write responses.

SCENARIO 7 (process_status_polling):
Use Process action=Spawn to run: find /usr/share -type f 2>/dev/null
Then call Process action=Status on that process_id 3 times in a row. Each Status call returns the full accumulated stdout. Report the stdout_bytes value from each Status call.

SCENARIO 8 (files_list_unbounded):
Use Files action=List on path '/usr/lib'. Report the number of entries returned.

SCENARIO 9 (editwrite_overwrite_triple_echo):
Now overwrite the same /tmp/twa_large.hpp from scenario 5 with different 150-line content. For an overwrite, the response includes old_lines (150 lines), new_lines (150 lines), AND diff_preview (300 lines). Report the sizes of old_lines, new_lines, and diff_preview in the response.

SCENARIO 10 (lsp_workspace_symbol):
Use the lsp tool with operation=workspace_symbol and query=''. Report the number of symbols returned.

After completing all 10 scenarios, output AUDIT_COMPLETE and a brief size summary.)";
}

std::string formatBytes(size_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / 1024.0) << " KB";
        return ss.str();
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    return ss.str();
}

std::string flagSeverity(size_t bytes) {
    if (bytes > 256 * 1024) return " [🔴 CRITICAL >256KB]";
    if (bytes > 64 * 1024)  return " [🟠 HIGH >64KB]";
    if (bytes > 16 * 1024)  return " [🟡 MEDIUM >16KB]";
    if (bytes > 4 * 1024)   return " [🟢 NOTE >4KB]";
    return "";
}

} // namespace

std::string TokenWasteAudit::getId() const { return "token_waste"; }

std::string TokenWasteAudit::getDescription() const {
    return "Verify 10 token-waste scenarios (grep/glob/read/process/editwrite/artifact/status-poll/list/overwrite/lsp) via a live agent run; measures provider-reported token costs";
}

shared::AuditResult TokenWasteAudit::run(const std::vector<std::string>& args) {
    shared::AuditResult result;
    result.auditId = getId();
    result.passed = false;

    ParsedArgs parsed;
    try {
        parsed = parseArgs(args);
    } catch (const std::exception& e) {
        if (std::string(e.what()) == "help") {
            result.exitCode = 0;
            result.passed = true;
            return result;
        }
        result.exitCode = 2;
        result.output = std::string("Argument error: ") + e.what();
        return result;
    }

    Panic::init();
    EnvLoader::load(".env.local");

    const auto originalConfig = ConfigLoader::instance().getConfig();

    auto cleanup = [&]() {
        Harness::instance().shutdown();
        ConfigLoader::instance().updateConfig(originalConfig);
    };

    std::cout << "[TokenWasteAudit] Provider: " << parsed.providerId
              << "  Model: " << parsed.modelId;
    if (!parsed.modelVariant.empty()) std::cout << " (" << parsed.modelVariant << ")";
    std::cout << "\n[TokenWasteAudit] Timeout: " << parsed.timeoutSeconds << "s\n";
    std::cout << "[TokenWasteAudit] Scenarios: 10 (grep/glob/read/process/editwrite/artifact/status-poll/list/overwrite/lsp)\n\n";

    try {
        auto& harness = Harness::instance();
        harness.init();

        auto cfg = ConfigLoader::instance().getConfig();
        cfg.defaultProviderId = parsed.providerId;
        cfg.defaultModelId = parsed.modelId;
        cfg.defaultModelVariant = parsed.modelVariant;
        cfg.defaultLeadPersona = parsed.persona;
        cfg.dangerouslySkipPermissions = true;
        cfg.mcpServers.clear();
        ConfigLoader::instance().updateConfig(cfg);

        provider::ProviderRegistry::instance().getProvider(parsed.providerId);

        const std::string workingDir = std::filesystem::current_path().string();
        const std::string threadId = harness.newThread({}, workingDir, parsed.persona);
        if (threadId.empty()) {
            cleanup();
            result.exitCode = 1;
            result.output = "Failed to create harness thread";
            return result;
        }
        std::cout << "[TokenWasteAudit] Thread: " << threadId << "\n";

        if (!parsed.modelVariant.empty()) {
            harness.switchModel(parsed.providerId, parsed.modelId, parsed.modelVariant);
        } else {
            harness.switchModel(parsed.providerId, parsed.modelId);
        }

        AuditState state;
        std::string leadAgentId;
        std::mutex leadIdMtx;
        std::condition_variable leadIdCv;
        bool leadIdReady = false;

        int subId = harness.subscribe([&](const AppEvent& ev) {
            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;

                if constexpr (std::is_same_v<T, AgentSpawned>) {
                    if (e.parentId.empty()) {
                        std::lock_guard<std::mutex> lk(leadIdMtx);
                        leadAgentId = e.agentId;
                        leadIdReady = true;
                        leadIdCv.notify_one();
                        std::cout << "[TokenWasteAudit] Lead agent spawned: " << e.agentId << "\n";
                    }
                } else if constexpr (std::is_same_v<T, AgentToolCall>) {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.lastToolName = e.toolName;
                    std::cout << "\n[TOOL CALL] " << e.toolName
                              << " args=" << e.toolArgs.substr(0, 120)
                              << (e.toolArgs.size() > 120 ? "..." : "") << "\n";
                } else if constexpr (std::is_same_v<T, AgentTurnCompleted>) {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    for (const auto& msg : e.turn.messages) {
                        for (const auto& part : msg.content) {
                            if (const auto* tc = std::get_if<ToolCallContent>(&part)) {
                                state.lastToolName = tc->name;
                            } else if (const auto* tr = std::get_if<ToolResultContent>(&part)) {
                                ToolResultRecord rec;
                                rec.toolName = state.lastToolName;
                                rec.resultBytes = tr->result.size();
                                rec.success = tr->success;
                                // promptDelta/ctxAfter filled in by next AgentMetricsStreamed
                                state.toolResults.push_back(rec);
                                std::cout << "[TOOL RESULT] tool=" << std::left << std::setw(14) << rec.toolName
                                          << "  bytes=" << std::right << std::setw(9) << formatBytes(rec.resultBytes)
                                          << "  (tokens pending...)"
                                          << flagSeverity(rec.resultBytes) << "\n";
                            }
                        }
                    }
                    if (e.aggregateMetrics.tokens.total > 0) {
                        state.totalTokens = e.aggregateMetrics.tokens.total;
                        state.totalInputTokens = e.aggregateMetrics.tokens.prompt;
                        state.totalOutputTokens = e.aggregateMetrics.tokens.completion;
                    }
                } else if constexpr (std::is_same_v<T, AgentMetricsStreamed>) {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    const uint32_t p = e.metrics.tokens.prompt;
                    if (p > 0) {
                        const uint32_t delta = (p > state.prevMetricsPrompt)
                                               ? (p - state.prevMetricsPrompt) : 0;
                        // Assign tokens to all pending tool results (ctxAfter == 0)
                        for (auto& rec : state.toolResults) {
                            if (rec.ctxAfter == 0) {
                                rec.promptDelta = delta;
                                rec.ctxAfter = p;
                                std::cout << "  [tokens] tool=" << rec.toolName
                                          << " +tokens=" << delta
                                          << " ctx=" << p << "\n";
                            }
                        }
                        state.prevMetricsPrompt = p;
                    }
                    if (e.metrics.tokens.total > state.totalTokens) {
                        state.totalTokens = e.metrics.tokens.total;
                        state.totalInputTokens = e.metrics.tokens.prompt;
                        state.totalOutputTokens = e.metrics.tokens.completion;
                    }
                } else if constexpr (std::is_same_v<T, AgentText>) {
                    std::cout << e.delta << std::flush;
                } else if constexpr (std::is_same_v<T, AgentThinking>) {
                    // suppress thinking in audit output
                } else if constexpr (std::is_same_v<T, AgentFinished>) {
                    if (e.agentId == leadAgentId) {
                        std::lock_guard<std::mutex> lk(state.mtx);
                        state.done = true;
                        state.cv.notify_one();
                    }
                } else if constexpr (std::is_same_v<T, AgentError>) {
                    if (e.agentId == leadAgentId) {
                        std::lock_guard<std::mutex> lk(state.mtx);
                        state.done = true;
                        state.hadError = true;
                        state.errorMessage = e.message;
                        state.cv.notify_one();
                    }
                } else if constexpr (std::is_same_v<T, PermissionEscalationRequest>) {
                    std::cout << "[PERM] auto-approving: " << e.command << "\n";
                    harness.resolvePermissionEscalation(e.requestId, PermissionResponse::AllowAlways);
                }
            }, ev);
        });

        const std::string prompt = buildScenarioPrompt();
        std::cout << "> [Sending audit prompt to " << parsed.providerId << "/" << parsed.modelId << "]\n\n";
        harness.send(prompt);

        {
            std::unique_lock<std::mutex> lk(leadIdMtx);
            if (!leadIdCv.wait_for(lk, kAgentSpawnTimeout, [&] {
                if (leadIdReady) return true;
                const std::string focused = harness.focusedAgentId();
                if (!focused.empty()) { leadAgentId = focused; leadIdReady = true; return true; }
                return false;
            })) {
                harness.unsubscribe(subId);
                cleanup();
                result.exitCode = 1;
                result.output = "Timed out waiting for lead agent spawn";
                return result;
            }
        }

        std::cout << "[TokenWasteAudit] Lead agent: " << leadAgentId << "\n";

        const auto timeout = std::chrono::seconds(parsed.timeoutSeconds);
        {
            std::unique_lock<std::mutex> lk(state.mtx);
            if (!state.cv.wait_for(lk, timeout, [&] { return state.done; })) {
                state.hadError = true;
                state.errorMessage = "Timed out waiting for agent completion (" +
                                     std::to_string(parsed.timeoutSeconds) + "s)";
            }
        }

        harness.unsubscribe(subId);

        std::ostringstream report;
        report << "\n=== TOKEN WASTE AUDIT REPORT ===\n";
        report << "Provider:  " << parsed.providerId << "\n";
        report << "Model:     " << parsed.modelId;
        if (!parsed.modelVariant.empty()) report << " (" << parsed.modelVariant << ")";
        report << "\n";
        report << "Error:     " << (state.hadError ? state.errorMessage : "none") << "\n";
        report << "Total tokens used:   " << state.totalTokens
               << " (in=" << state.totalInputTokens
               << " out=" << state.totalOutputTokens << ")\n";
        report << "\n--- Tool Result Token Costs (provider-reported) ---\n";
        report << std::left  << std::setw(4)  << "#"
               << std::setw(3)  << "st"
               << std::setw(22) << "tool"
               << std::right
               << std::setw(10) << "bytes"
               << std::setw(9)  << "+tokens"
               << std::setw(9)  << "ctx_tot"
               << "  flag\n"
               << std::string(72, '-') << "\n";

        size_t totalToolResultBytes = 0;
        uint32_t totalPromptDelta = 0;
        for (size_t i = 0; i < state.toolResults.size(); ++i) {
            const auto& rec = state.toolResults[i];
            totalToolResultBytes += rec.resultBytes;
            totalPromptDelta += rec.promptDelta;
            report << std::left  << std::setw(4)  << (i + 1)
                   << std::setw(3)  << (rec.success ? "OK" : "ERR")
                   << std::setw(22) << rec.toolName
                   << std::right
                   << std::setw(10) << formatBytes(rec.resultBytes)
                   << std::setw(9)  << rec.promptDelta
                   << std::setw(9)  << rec.ctxAfter
                   << flagSeverity(rec.resultBytes) << "\n";
        }
        report << std::string(72, '-') << "\n";

        report << "Tool calls observed:       " << state.toolResults.size() << "\n";
        report << "Total tool result bytes:   " << formatBytes(totalToolResultBytes) << "\n";
        report << "Total +tokens (deltas):    " << totalPromptDelta << "\n";

        if (!state.toolResults.empty()) {
            auto biggestBytes = std::max_element(state.toolResults.begin(), state.toolResults.end(),
                [](const ToolResultRecord& a, const ToolResultRecord& b) {
                    return a.resultBytes < b.resultBytes;
                });
            auto biggestTokens = std::max_element(state.toolResults.begin(), state.toolResults.end(),
                [](const ToolResultRecord& a, const ToolResultRecord& b) {
                    return a.promptDelta < b.promptDelta;
                });
            report << "Largest by bytes:          " << formatBytes(biggestBytes->resultBytes)
                   << "  [tool=" << biggestBytes->toolName << "]\n";
            report << "Largest by tokens:         " << biggestTokens->promptDelta << " tok"
                   << "  [tool=" << biggestTokens->toolName << "]\n";
        }

        report << "================================\n";

        result.output = report.str();
        result.exitCode = state.hadError ? 1 : 0;
        result.passed = !state.hadError && !state.toolResults.empty();

        cleanup();
        return result;

    } catch (const std::exception& e) {
        cleanup();
        result.exitCode = 1;
        result.output = std::string("Fatal exception: ") + e.what();
        return result;
    }
}

} // namespace firmius::audits
