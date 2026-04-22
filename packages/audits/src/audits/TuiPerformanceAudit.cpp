#include "audits/TuiPerformanceAudit.hpp"
#include "persistence/ThreadManager.hpp"
#include "persistence/Journaler.hpp"
#include "AgentRegistry.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <ftxui/screen/screen.hpp>
#include <iomanip>
#include <cstring>
#include <limits>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::tui;

namespace {

std::string randomString(std::mt19937 &rng, int len) {
    static const char charset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz ";
    std::string result;
    result.reserve(len);
    for (int i = 0; i < len; ++i) {
        result += charset[rng() % (sizeof(charset) - 1)];
    }
    return result;
}

void emitTextChurn(TuiState &tuiState, int frame) {
    AgentText ev;
    ev.agentId = "agent-1";
    ev.delta = "Token " + std::to_string(frame);
    tuiState.handleAppEvent(AppEvent(ev));
}

void emitFileEditChurn(TuiState &tuiState, int frame) {
    AgentToolCall tool;
    tool.agentId = "agent-1";
    tool.toolCallId = "edit-" + std::to_string(frame);
    tool.toolName = "file_edit";
    tool.toolArgs = "{\"path\":\"src/demo_" + std::to_string(frame % 8) + ".cpp\"}";
    tuiState.handleAppEvent(AppEvent(tool));

    AgentFileEdited edited;
    edited.agentId = "agent-1";
    edited.toolCallId = tool.toolCallId;
    edited.path = "src/demo_" + std::to_string(frame % 8) + ".cpp";
    edited.addedLines = 18;
    edited.removedLines = 11;
    edited.diffPreview =
        "@@ -40,8 +40,15 @@\n"
        "-old_call();\n"
        "+new_call();\n"
        "+render_preview();\n"
        " context\n"
        "@@ -88,4 +95,10 @@\n"
        "-return false;\n"
        "+return true;\n"
        "+log_edit(frame);\n";
    tuiState.handleAppEvent(AppEvent(edited));
}

void emitProcessChurn(TuiState &tuiState, int frame) {
    const std::string processId = "proc-" + std::to_string(frame % 12);
    const std::string toolCallId = "proc-tool-" + std::to_string(frame % 12);

    AgentToolCall tool;
    tool.agentId = "agent-1";
    tool.toolCallId = toolCallId;
    tool.toolName = "Process";
    tool.toolArgs = "{\"action\":\"Spawn\",\"command\":\"tail -f build.log\"}";
    tuiState.handleAppEvent(AppEvent(tool));

    AgentProcessSpawned spawned;
    spawned.agentId = "agent-1";
    spawned.processId = processId;
    spawned.toolCallId = toolCallId;
    spawned.command = "tail -f build.log";
    tuiState.handleAppEvent(AppEvent(spawned));

    AgentProcessOutput output;
    output.agentId = "agent-1";
    output.processId = processId;
    output.output = "stdout line " + std::to_string(frame) + "\nmore output\nwarning: churn";
    output.isStderr = (frame % 5 == 0);
    output.finished = false;
    output.exitCode = -1;
    output.durationMs = static_cast<double>(frame) * 7.0;
    tuiState.handleAppEvent(AppEvent(output));
}

void emitSubagentChurn(TuiState &tuiState, int frame) {
    const std::string toolCallId = "delegate-" + std::to_string(frame % 16);
    const std::string childId = "subagent-" + std::to_string(frame % 16);

    AgentToolCall tool;
    tool.agentId = "agent-1";
    tool.toolCallId = toolCallId;
    tool.toolName = "Delegate";
    tool.toolArgs =
        "{\"action\":\"Spawn\",\"name\":\"forge\",\"title\":\"Forge Worker\",\"task\":\"Inspect src tree\"}";
    tuiState.handleAppEvent(AppEvent(tool));

    AgentSpawned spawned;
    spawned.agentId = childId;
    spawned.parentId = "agent-1";
    spawned.friendlyName = "forge";
    spawned.title = "Forge Worker";
    spawned.persistHistory = true;
    tuiState.handleAppEvent(AppEvent(spawned));

    AgentThinking thinking;
    thinking.agentId = childId;
    thinking.parentId = "agent-1";
    thinking.delta = "checking files and narrowing edit surface";
    tuiState.handleAppEvent(AppEvent(thinking));

    AgentToolCall childTool;
    childTool.agentId = childId;
    childTool.parentId = "agent-1";
    childTool.toolCallId = "child-read-" + std::to_string(frame % 16);
    childTool.toolName = "Files";
    childTool.toolArgs = "{\"action\":\"Read\",\"path\":\"src/main.cpp\"}";
    tuiState.handleAppEvent(AppEvent(childTool));
}

struct ScenarioResult {
    std::string name;
    int turns = 0;
    int frames = 0;
    long long durationMs = 0;
    double fps = 0.0;
    double avgFrameMs = 0.0;
    std::string summary;
};

ScenarioResult runScenario(const std::string &name,
                           int turns,
                           const std::string &threadsBase,
                           const std::string &tempDir,
                           const std::function<void(TuiState &, int)> &perFrame,
                           TuiPerformanceAudit &audit) {
    ScenarioResult result;
    result.name = name;
    result.turns = turns;
    result.frames = 20;

    const std::string threadId = audit.generateStressThread(turns, threadsBase);

    auto &harness = Harness::instance();
    ::setenv("HOME", tempDir.c_str(), 1);
    harness.init();

    ThreadManager tm(threadsBase);
    ThreadMetadata meta = tm.getMetadata(threadId);

    auto &tuiState = TuiState::instance();
    tuiState.init(harness, meta, "agent-1");
    tuiState.setViewMode(TuiState::ViewMode::Chat);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Full(), ftxui::Dimension::Full());
    (void)screen;
    auto root = tuiState.root();

    for (int i = 0; i < 5; ++i) {
        root->Render();
    }

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < result.frames; ++i) {
        perFrame(tuiState, i);
        root->Render();
    }
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    result.durationMs = duration.count();
    result.fps = result.durationMs > 0
                     ? (result.frames * 1000.0) / static_cast<double>(result.durationMs)
                     : std::numeric_limits<double>::infinity();
    result.avgFrameMs = result.frames > 0
                            ? static_cast<double>(result.durationMs) / static_cast<double>(result.frames)
                            : 0.0;
    result.summary = tuiState.exitSummaryText();

    tuiState.shutdown();
    harness.shutdown();
    return result;
}

} // namespace

std::string TuiPerformanceAudit::getId() const { return "tui_performance"; }

std::string TuiPerformanceAudit::getDescription() const {
    return "Benchmark TUI performance with transcript, file-edit, process, and subagent stress scenarios";
}

std::string TuiPerformanceAudit::generateStressThread(int turns, const std::string& tempDir) {
    ThreadManager tm(tempDir);
    ThreadMetadata meta;
    meta.title = "Stress Test " + std::to_string(turns) + " Turns";

    const std::string threadId = tm.createThread(meta);

    std::string agentId = "agent-1";
    std::map<std::string, AgentManifestEntry> manifest;
    manifest[agentId] = {"aster", "", "aster", "Lead", true};
    tm.writeAgentManifest(threadId, manifest);

    std::vector<AgentTurn> history;
    std::mt19937 perf_rng(1337 + turns);
    std::uniform_int_distribution<int> dist(0, 2);

    for (int i = 0; i < turns; ++i) {
        AgentTurn turn;
        turn.turnId = "turn-" + std::to_string(i);

        Message userMsg;
        userMsg.id = "msg-u-" + std::to_string(i);
        userMsg.role = Role::User;
        userMsg.content.push_back(TextContent{randomString(perf_rng, 50 + (perf_rng() % 200))});
        turn.messages.push_back(userMsg);

        Message asstMsg;
        asstMsg.id = "msg-a-" + std::to_string(i);
        asstMsg.role = Role::Assistant;

        int type = dist(perf_rng);
        if (type == 0) {
            asstMsg.content.push_back(TextContent{randomString(perf_rng, 100 + (perf_rng() % 500))});
        } else if (type == 1) {
            asstMsg.content.push_back(ToolCallContent{"call-" + std::to_string(i), "process_execute", "{\"command\":\"ls -R\"}"});
            Message resultMsg;
            resultMsg.id = "msg-r-" + std::to_string(i);
            resultMsg.role = Role::ToolResult;
            resultMsg.content.push_back(ToolResultContent{"call-" + std::to_string(i), randomString(perf_rng, 200 + (perf_rng() % 1000)), true, "", ""});
            turn.messages.push_back(resultMsg);
        } else {
            asstMsg.content.push_back(ThinkingContent{randomString(perf_rng, 100 + (perf_rng() % 300)), ""});
            asstMsg.content.push_back(TextContent{randomString(perf_rng, 100 + (perf_rng() % 200))});
        }

        turn.messages.push_back(asstMsg);
        history.push_back(turn);
    }

    Journaler journal(threadId, agentId);
    journal.rewriteJournal(history);
    return threadId;
}

shared::AuditResult TuiPerformanceAudit::run(const std::vector<std::string>& args) {
    (void)args;
    AuditResult result;
    result.auditId = getId();
    result.passed = true;

    ::setenv("FIRMIUS_TUI_STARTUP_PROFILE", "1", 1);

    std::cout << "🚀 STARTING TUI PERFORMANCE AUDIT (EXPANDED LIVE STRESS)" << std::endl;

    std::string tempDirTemplate = "/tmp/firmius_perf_XXXXXX";
    char *path = ::strdup(tempDirTemplate.c_str());
    if (::mkdtemp(path) == nullptr) {
        std::cerr << "Failed to create temp directory" << std::endl;
        result.passed = false;
        if (path) ::free(path);
        return result;
    }
    std::string tempDir(path);
    ::free(path);

    std::filesystem::path threadsPath = std::filesystem::path(tempDir) / "threads";
    std::filesystem::create_directories(threadsPath);
    const std::string threadsBase = threadsPath.string();

    TuiPerformanceAudit audit;
    const std::vector<ScenarioResult> results = {
        runScenario("transcript_churn_123", 123, threadsBase, tempDir, emitTextChurn, audit),
        runScenario("file_edit_heavy_123", 123, threadsBase, tempDir, emitFileEditChurn, audit),
        runScenario("process_heavy_123", 123, threadsBase, tempDir, emitProcessChurn, audit),
        runScenario("subagent_heavy_123", 123, threadsBase, tempDir, emitSubagentChurn, audit),
        runScenario("mixed_500", 500, threadsBase, tempDir,
                    [](TuiState &state, int frame) {
                        emitTextChurn(state, frame);
                        emitFileEditChurn(state, frame);
                        emitProcessChurn(state, frame);
                        emitSubagentChurn(state, frame);
                    }, audit),
        runScenario("mixed_1000", 1000, threadsBase, tempDir,
                    [](TuiState &state, int frame) {
                        emitTextChurn(state, frame);
                        emitFileEditChurn(state, frame);
                        emitProcessChurn(state, frame);
                        emitSubagentChurn(state, frame);
                    }, audit)};

    for (const auto &scenario : results) {
        std::cout << "\n=== Scenario: " << scenario.name << " ===" << std::endl;
        std::cout << "Turns: " << scenario.turns << "  Frames: " << scenario.frames
                  << "  Total: " << scenario.durationMs << "ms" << std::endl;
        std::cout << "FPS: " << std::fixed << std::setprecision(2) << scenario.fps
                  << "  Avg Frame: " << scenario.avgFrameMs << "ms" << std::endl;
        std::cout << scenario.summary << std::endl;
    }

    std::filesystem::remove_all(threadsPath);
    return result;
}

} // namespace firmius::audits
