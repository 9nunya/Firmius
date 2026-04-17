#include "audits/WebSearchAudit.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

namespace {
constexpr auto kLeadSpawnTimeout = std::chrono::seconds(20);
constexpr auto kAuditCompletionTimeout = std::chrono::seconds(180);

struct AuditState {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool hadError = false;
    std::string errorMessage;
    int subagentSpawns = 0;
    int subagentCompletions = 0;
    bool sawWebSearchToolCall = false;
    bool sawAnyAgentText = false;
    bool sawAgentTextAfterWebSearch = false;
};

} // namespace

std::string WebSearchAudit::getId() const { return "web_search"; }

std::string WebSearchAudit::getDescription() const {
    return "Verify web_search tool works by spawning a lead agent to search the web";
}

AuditResult WebSearchAudit::run(const std::vector<std::string>& args) {
    AuditResult result;
    result.auditId = getId();

    std::string query;
    bool expectFailure = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "Usage: firmius_audit --audit web_search [--expect-failure] --query <query>\n";
            result.exitCode = 0;
            result.passed = true;
            return result;
        }
        if (args[i] == "--expect-failure") {
            expectFailure = true;
        }
        if (args[i] == "--query" && i + 1 < args.size()) {
            query = args[++i];
        }
    }

    if (query.empty() && !expectFailure) {
        std::cerr << "Error: --query argument is required (or use --expect-failure)\n";
        std::cerr << "Usage: firmius_audit --audit web_search [--expect-failure] --query <query>\n";
        result.exitCode = 1;
        result.passed = false;
        return result;
    }

    std::cout << "WebSearchAudit: searching \"" << query << "\" via harness lead agent\n";

    Panic::init();
    EnvLoader::load(".env.local");

    auto& harness = Harness::instance();
    harness.init();

    HostCreationOptions opts;
    opts.type = HostType::Local;
    opts.containerName = "";
    opts.deleteOnExit = false;

    const std::string workingDir = std::filesystem::current_path().string();
    std::string threadId = harness.newThread(opts, workingDir, "lead");
    if (threadId.empty()) {
        std::cerr << "Failed to create harness thread\n";
        result.exitCode = 1;
        result.passed = false;
        result.output = "Failed to create thread";
        harness.shutdown();
        return result;
    }
    std::cout << "Harness thread created: " << threadId << "\n";

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
                }
            } else if constexpr (std::is_same_v<T, AgentError>) {
                if (e.agentId == leadAgentId) {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.done = true;
                    state.hadError = true;
                    state.errorMessage = e.message;
                    state.cv.notify_one();
                }
            } else if constexpr (std::is_same_v<T, AgentFinished>) {
                if (e.agentId == leadAgentId) {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.done = true;
                    state.cv.notify_one();
                } else {
                    state.subagentCompletions++;
                }
            } else if constexpr (std::is_same_v<T, AgentText>) {
                std::cout << e.delta << std::flush;
                std::lock_guard<std::mutex> lk(state.mtx);
                state.sawAnyAgentText = true;
                if (state.sawWebSearchToolCall) {
                    state.sawAgentTextAfterWebSearch = true;
                }
            } else if constexpr (std::is_same_v<T, AgentThinking>) {
                // skip thinking output in audit
            } else if constexpr (std::is_same_v<T, AgentToolCall>) {
                std::cout << "\n[Tool] " << e.toolName << std::flush;
                if (e.toolName == "web_search") {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.sawWebSearchToolCall = true;
                }
            }
        }, ev);
    });

    std::string prompt = "Use the web_search tool to search for: " + query;
    std::cout << "\n> User: " << prompt << "\n";
    harness.send(prompt);
    {
        std::unique_lock<std::mutex> lk(leadIdMtx);
        if (!leadIdCv.wait_for(lk, kLeadSpawnTimeout, [&] {
                if (leadIdReady) {
                    return true;
                }
                const std::string focused = harness.focusedAgentId();
                if (!focused.empty()) {
                    leadAgentId = focused;
                    leadIdReady = true;
                    return true;
                }
                return false;
            })) {
            harness.unsubscribe(subId);
            harness.shutdown();
            std::cerr << "AUDIT FAILED: timed out waiting for lead agent spawn\n";
            result.exitCode = 1;
            result.passed = false;
            result.output = "lead agent spawn timeout";
            return result;
        }
    }
    std::cout << "Lead agent: " << leadAgentId << "\n";

    {
        std::unique_lock<std::mutex> lk(state.mtx);
        if (!state.cv.wait_for(lk, kAuditCompletionTimeout, [&] { return state.done; })) {
            state.hadError = true;
            state.errorMessage = "timed out waiting for lead agent completion";
        }
    }

    harness.unsubscribe(subId);

    std::ostringstream out;
    out << "query=" << query << "\n";
    out << "lead_agent=" << leadAgentId << "\n";
    out << "error=" << (state.hadError ? state.errorMessage : "none") << "\n";
    out << "subagent_spawns=" << state.subagentSpawns << "\n";
    out << "subagent_completions=" << state.subagentCompletions << "\n";
    out << "saw_web_search_tool_call=" << (state.sawWebSearchToolCall ? "true" : "false") << "\n";
    out << "saw_any_agent_text=" << (state.sawAnyAgentText ? "true" : "false") << "\n";
    out << "saw_agent_text_after_web_search=" << (state.sawAgentTextAfterWebSearch ? "true" : "false") << "\n";

    harness.shutdown();

    if (expectFailure) {
        // Error mode: we expect failure (e.g., no providers configured)
        if (state.hadError) {
            std::cout << "\nAUDIT PASSED (error path): web_search failed as expected: " << state.errorMessage << "\n";
            result.exitCode = 0;
            result.passed = true;
            result.output = out.str();
            return result;
        }
        std::cerr << "AUDIT FAILED (error path): web_search succeeded but failure was expected\n";
        result.exitCode = 1;
        result.passed = false;
        result.output = out.str();
        return result;
    }

    // Normal mode: we expect success
    if (state.hadError) {
        std::cerr << "AUDIT FAILED: Agent error: " << state.errorMessage << "\n";
        result.exitCode = 1;
        result.passed = false;
        result.output = out.str();
        return result;
    }
    if (!state.sawWebSearchToolCall) {
        std::cerr << "AUDIT FAILED: lead agent never invoked web_search tool\n";
        result.exitCode = 1;
        result.passed = false;
        result.output = out.str();
        return result;
    }
    if (!state.sawAnyAgentText || !state.sawAgentTextAfterWebSearch) {
        std::cerr << "AUDIT FAILED: no agent result text observed after web_search tool call\n";
        result.exitCode = 1;
        result.passed = false;
        result.output = out.str();
        return result;
    }

    std::cout << "\nAUDIT PASSED: web_search completed without error\n";
    result.exitCode = 0;
    result.passed = true;
    result.output = out.str();
    return result;
}

} // namespace firmius::audits
