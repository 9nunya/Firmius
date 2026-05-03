#include "audits/TuiRuntimeStressAudit.hpp"

#include "TUIState.hpp"
#include "components/Markdown.hpp"
#include "harness/Harness.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::tui;

namespace {

using Clock = std::chrono::steady_clock;

struct ProcSnapshot {
  long rss_kb = 0;
  long vsz_kb = 0;
  long threads = 0;
};

ProcSnapshot procSnap() {
  ProcSnapshot s;
#ifdef __linux__
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      std::sscanf(line.c_str(), "VmRSS: %ld", &s.rss_kb);
    } else if (line.rfind("VmSize:", 0) == 0) {
      std::sscanf(line.c_str(), "VmSize: %ld", &s.vsz_kb);
    } else if (line.rfind("Threads:", 0) == 0) {
      std::sscanf(line.c_str(), "Threads: %ld", &s.threads);
    }
  }
#endif
  return s;
}

std::string randomString(std::mt19937 &rng, int len) {
  static const char charset[] =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz "
      "       \n";
  std::string out;
  out.reserve(static_cast<size_t>(len));
  for (int i = 0; i < len; ++i) {
    out.push_back(charset[rng() % (sizeof(charset) - 1)]);
  }
  return out;
}

struct ScenarioConfig {
  std::string name;
  int turns = 3000;
  int frames = 260;
  int width = 180;
  int height = 50;
};

struct ScenarioResult {
  std::string name;
  int turns = 0;
  int frames = 0;
  int width = 0;
  int height = 0;
  double wall_ms = 0.0;
  double fps = 0.0;
  double frame_avg_ms = 0.0;
  double frame_p95_ms = 0.0;
  double frame_max_ms = 0.0;
  double rss_start_mb = 0.0;
  double rss_peak_mb = 0.0;
  double rss_end_mb = 0.0;
  std::string profiling;
};

void emitDiffEvent(TuiState &state, int frame) {
  AgentToolCall tool;
  tool.agentId = "agent-1";
  tool.toolCallId = "edit-" + std::to_string(frame);
  tool.toolName = "file_edit";
  tool.toolArgs = "{\"path\":\"src/demo_" + std::to_string(frame % 64) +
                 ".cpp\",\"old_str\":\"foo\",\"new_str\":\"bar\"}";
  state.handleAppEvent(AppEvent(tool));

  AgentFileEdited edited;
  edited.agentId = "agent-1";
  edited.toolCallId = tool.toolCallId;
  edited.path = "src/demo_" + std::to_string(frame % 64) + ".cpp";
  edited.addedLines = 9 + (frame % 20);
  edited.removedLines = 5 + (frame % 11);
  edited.diffPreview =
      "@@ -48,7 +48,15 @@\n"
      "-old_path();\n"
      "+new_path();\n"
      "+apply_fix();\n"
      "+assert(condition);\n"
      " context\n"
      "@@ -115,5 +123,9 @@\n"
      "-return false;\n"
      "+return true;\n";
  state.handleAppEvent(AppEvent(edited));
}

void emitProcessEvent(TuiState &state, int frame) {
  const std::string processId = "proc-" + std::to_string(frame % 24);
  const std::string toolCallId = "proc-call-" + std::to_string(frame % 24);

  AgentToolCall call;
  call.agentId = "agent-1";
  call.toolCallId = toolCallId;
  call.toolName = "process_execute";
  call.toolArgs =
      "{\"command\":\"tail -f build.log\",\"timeout\":120000}";
  state.handleAppEvent(AppEvent(call));

  AgentProcessSpawned spawned;
  spawned.agentId = "agent-1";
  spawned.processId = processId;
  spawned.toolCallId = toolCallId;
  spawned.command = "tail -f build.log";
  state.handleAppEvent(AppEvent(spawned));

  AgentProcessOutput out;
  out.agentId = "agent-1";
  out.processId = processId;
  out.output = "line " + std::to_string(frame) +
               " compiling...\nstdout burst\nstderr burst\n";
  out.isStderr = (frame % 4 == 0);
  out.finished = (frame % 25 == 0);
  out.exitCode = out.finished ? 0 : -1;
  out.durationMs = static_cast<double>(frame) * 4.0;
  state.handleAppEvent(AppEvent(out));
}

void emitSubagentEvent(TuiState &state, int frame) {
  const std::string childId = "sub-" + std::to_string(frame % 20);
  const std::string toolCallId = "delegate-" + std::to_string(frame % 20);

  AgentToolCall parentCall;
  parentCall.agentId = "agent-1";
  parentCall.toolCallId = toolCallId;
  parentCall.toolName = "delegate";
  parentCall.toolArgs =
      "{\"name\":\"forge\",\"task\":\"scan repository\"}";
  state.handleAppEvent(AppEvent(parentCall));

  AgentSpawned spawned;
  spawned.agentId = childId;
  spawned.parentId = "agent-1";
  spawned.friendlyName = "forge";
  spawned.title = "Forge Worker";
  spawned.persistHistory = true;
  state.handleAppEvent(AppEvent(spawned));

  AgentThinking thinking;
  thinking.agentId = childId;
  thinking.parentId = "agent-1";
  thinking.delta = "reading files and planning edits";
  state.handleAppEvent(AppEvent(thinking));

  AgentText text;
  text.agentId = childId;
  text.parentId = "agent-1";
  text.delta = "I found risky callsites in parser and scheduler";
  state.handleAppEvent(AppEvent(text));
}

void emitToolCallChunk(TuiState &state, int frame) {
  AgentToolCallChunk chunk;
  chunk.index = static_cast<uint32_t>(frame);
  chunk.agentId = "agent-1";
  chunk.toolCallId = "chunked-" + std::to_string(frame % 16);
  chunk.nameDelta = (frame % 2 == 0) ? "web_" : "search";
  chunk.argsDelta = "{\"query\":\"firmius " + std::to_string(frame) + "\"}";
  state.handleAppEvent(AppEvent(chunk));
}

std::string makeTurnToolName(int i) {
  static const std::vector<std::string> names = {
      "file_edit",       "file_read",      "process_execute", "process_spawn",
      "grep_search",     "find_by_name",   "run_command",     "code_search",
      "web_search",      "web_fetch",      "subagent",        "subagent_wait",
      "artifact_write",  "artifact_list",  "python_execute",  "update_plan",
      "read_file",       "apply_patch",    "browser_preview", "command_status"};
  return names[static_cast<size_t>(i % static_cast<int>(names.size()))];
}

std::string makeTurnToolArgs(const std::string &tool, int i) {
  if (tool == "file_edit")
    return "{\"path\":\"src/file_" + std::to_string(i % 128) +
           ".cpp\",\"old_str\":\"a\",\"new_str\":\"b\"}";
  if (tool == "process_execute")
    return "{\"command\":\"make -j8\"}";
  if (tool == "web_search")
    return "{\"query\":\"firmius tool rendering\"}";
  if (tool == "browser_preview")
    return "{\"url\":\"http://localhost:3000\",\"name\":\"Preview\"}";
  return "{\"id\":" + std::to_string(i) + "}";
}

std::string makeTurnToolResult(const std::string &tool, int i, std::mt19937 &rng) {
  if (tool == "file_edit") {
    return "@@ -10,6 +10,11 @@\n-old\n+new\n+added\n"
           "@@ -40,3 +45,6 @@\n-old2\n+new2\n";
  }
  if (tool == "process_execute") {
    return "$ make -j8\ncc -O2 src/a.cpp\ncc -O2 src/b.cpp\nlink done\n";
  }
  if (tool == "web_search") {
    return "[{\"title\":\"Firmius docs\",\"url\":\"https://example.com\"}]";
  }
  if (tool == "artifact_write") {
    return "{\"artifact_id\":\"art-" + std::to_string(i) +
           "\",\"status\":\"created\"}";
  }
  return randomString(rng, 280 + (rng() % 1200));
}

std::string generateStressThread(int turns, const std::string &threadsBase) {
  ThreadManager tm(threadsBase);
  ThreadMetadata meta;
  meta.title = "TUI Runtime Stress " + std::to_string(turns) + " turns";
  const std::string threadId = tm.createThread(meta);

  std::map<std::string, AgentManifestEntry> manifest;
  manifest["agent-1"] = {"aster", "", "aster", "Lead", true};
  tm.writeAgentManifest(threadId, manifest);

  std::vector<AgentTurn> history;
  history.reserve(static_cast<size_t>(turns));
  std::mt19937 rng(1337 + turns);

  for (int i = 0; i < turns; ++i) {
    AgentTurn turn;
    turn.turnId = "turn-" + std::to_string(i);

    Message user;
    user.id = "u-" + std::to_string(i);
    user.role = Role::User;
    user.content.push_back(TextContent{randomString(rng, 60 + (rng() % 180))});
    turn.messages.push_back(std::move(user));

    Message asst;
    asst.id = "a-" + std::to_string(i);
    asst.role = Role::Assistant;

    const std::string tool = makeTurnToolName(i);
    const std::string callId = "call-" + std::to_string(i);

    if (i % 3 == 0) {
      asst.content.push_back(ThinkingContent{randomString(rng, 120 + (rng() % 320)), ""});
    }
    asst.content.push_back(TextContent{randomString(rng, 100 + (rng() % 500))});
    asst.content.push_back(ToolCallContent{callId, tool, makeTurnToolArgs(tool, i)});
    turn.messages.push_back(std::move(asst));

    Message result;
    result.id = "r-" + std::to_string(i);
    result.role = Role::ToolResult;
    result.content.push_back(
        ToolResultContent{callId, makeTurnToolResult(tool, i, rng), true, "", ""});
    turn.messages.push_back(std::move(result));

    turn.metrics.tokens.prompt = 100 + (rng() % 2000);
    turn.metrics.tokens.completion = 100 + (rng() % 3000);
    turn.metrics.tokens.reasoning = (i % 3 == 0) ? (40 + (rng() % 800)) : 0;
    turn.metrics.tokens.total = turn.metrics.tokens.prompt + turn.metrics.tokens.completion +
                                turn.metrics.tokens.reasoning;
    turn.metrics.estimatedCostUsd = 0.000001 * turn.metrics.tokens.total;

    history.push_back(std::move(turn));
  }

  {
    Journaler journal(threadId, "agent-1");
    journal.rewriteJournal(history);
  }

  return threadId;
}

ScenarioResult runScenario(const ScenarioConfig &cfg, const std::string &threadsBase,
                           const std::string &tempHome) {
  ScenarioResult result;
  result.name = cfg.name;
  result.turns = cfg.turns;
  result.frames = cfg.frames;
  result.width = cfg.width;
  result.height = cfg.height;

  auto &harness = Harness::instance();
  ::setenv("HOME", tempHome.c_str(), 1);
  ::setenv("FIRMIUS_TUI_STARTUP_PROFILE", "1", 1);

  const std::string threadId = generateStressThread(cfg.turns, threadsBase);
  ThreadManager tm(threadsBase);
  ThreadMetadata meta = tm.getMetadata(threadId);

  harness.init();
  auto &state = TuiState::instance();
  state.init(harness, meta, "agent-1");
  state.setViewMode(TuiState::ViewMode::Chat);

  SetMarkdownWidth(std::max(20, cfg.width - 6));
  auto root = state.root();

  for (int i = 0; i < 8; ++i) {
    (void)root->Render();
  }

  std::vector<double> frameTimes;
  frameTimes.reserve(static_cast<size_t>(cfg.frames));

  const ProcSnapshot startSnap = procSnap();
  long peakRss = startSnap.rss_kb;

  const auto t0 = Clock::now();
  for (int i = 0; i < cfg.frames; ++i) {
    AgentText txt;
    txt.agentId = "agent-1";
    txt.delta = "token " + std::to_string(i);
    state.handleAppEvent(AppEvent(txt));

    emitDiffEvent(state, i);
    emitProcessEvent(state, i);
    emitSubagentEvent(state, i);
    emitToolCallChunk(state, i);

    auto f0 = Clock::now();
    (void)root->Render();
    auto f1 = Clock::now();
    frameTimes.push_back(
        std::chrono::duration<double, std::milli>(f1 - f0).count());

    if ((i % 10) == 0) {
      peakRss = std::max<long>(peakRss, procSnap().rss_kb);
    }
  }
  const auto t1 = Clock::now();

  const ProcSnapshot endSnap = procSnap();

  std::sort(frameTimes.begin(), frameTimes.end());
  const auto idx95 = static_cast<size_t>(
      std::min<int>(static_cast<int>(frameTimes.size()) - 1,
                    static_cast<int>(frameTimes.size() * 0.95)));

  result.wall_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  result.fps = result.wall_ms > 0.0
                   ? (static_cast<double>(cfg.frames) * 1000.0 / result.wall_ms)
                   : std::numeric_limits<double>::infinity();
  result.frame_avg_ms = frameTimes.empty()
                            ? 0.0
                            : (std::accumulate(frameTimes.begin(), frameTimes.end(),
                                               0.0) /
                               static_cast<double>(frameTimes.size()));
  result.frame_p95_ms = frameTimes.empty() ? 0.0 : frameTimes[idx95];
  result.frame_max_ms = frameTimes.empty() ? 0.0 : frameTimes.back();
  result.rss_start_mb = static_cast<double>(startSnap.rss_kb) / 1024.0;
  result.rss_peak_mb = static_cast<double>(peakRss) / 1024.0;
  result.rss_end_mb = static_cast<double>(endSnap.rss_kb) / 1024.0;
  result.profiling = state.exitSummaryText();

  state.shutdown();
  harness.shutdown();
  return result;
}

} // namespace

std::string TuiRuntimeStressAudit::getId() const { return "tui_runtime_stress"; }

std::string TuiRuntimeStressAudit::getDescription() const {
  return "Internal TUI runtime stress: diff renders, live process output, "
         "subagent/tool presentation churn across terminal-size matrix with "
         "frame and memory metrics";
}

shared::AuditResult
TuiRuntimeStressAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();
  result.passed = true;

  int turns = 3000;
  int frames = 260;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--turns" && i + 1 < args.size()) {
      turns = std::stoi(args[++i]);
    } else if (args[i] == "--frames" && i + 1 < args.size()) {
      frames = std::stoi(args[++i]);
    }
  }

  std::string tempTemplate = "/tmp/firmius_tui_runtime_XXXXXX";
  char *path = ::strdup(tempTemplate.c_str());
  if (::mkdtemp(path) == nullptr) {
    result.passed = false;
    result.exitCode = 1;
    result.output = "Failed to create temp dir";
    if (path)
      ::free(path);
    return result;
  }
  const std::string tempHome(path);
  ::free(path);

  std::filesystem::path threadsPath = std::filesystem::path(tempHome) / ".firmius/threads";
  std::filesystem::create_directories(threadsPath);
  const std::string threadsBase = threadsPath.string();

  const std::vector<std::pair<int, int>> sizes = {
      {100, 30}, {140, 40}, {180, 50}, {220, 60}};

  std::vector<ScenarioResult> results;
  results.reserve(sizes.size());

  std::cout << "Starting tui_runtime_stress turns=" << turns
            << " frames=" << frames << std::endl;
  for (const auto &[w, h] : sizes) {
    ScenarioConfig cfg;
    cfg.name = "tool_diff_process_matrix_" + std::to_string(w) + "x" +
               std::to_string(h);
    cfg.turns = turns;
    cfg.frames = frames;
    cfg.width = w;
    cfg.height = h;
    std::cout << "Running " << cfg.name << "..." << std::endl;
    results.push_back(runScenario(cfg, threadsBase, tempHome));
  }

  std::ostringstream out;
  out << "=== TUI Runtime Stress Results ===\n";
  out << "scenario\tturns\tframes\tsize\tfps\tavg_ms\tp95_ms\tmax_ms\t"
         "rss_start_mb\trss_peak_mb\trss_end_mb\n";

  for (const auto &r : results) {
    out << r.name << "\t" << r.turns << "\t" << r.frames << "\t" << r.width
        << "x" << r.height << "\t" << std::fixed << std::setprecision(2)
        << r.fps << "\t" << r.frame_avg_ms << "\t" << r.frame_p95_ms << "\t"
        << r.frame_max_ms << "\t" << r.rss_start_mb << "\t" << r.rss_peak_mb
        << "\t" << r.rss_end_mb << "\n";
  }

  out << "\n--- Profiling Summaries ---\n";
  for (const auto &r : results) {
    out << "\n[" << r.name << "]\n" << r.profiling << "\n";
  }

  result.output = out.str();

  std::error_code ec;
  std::filesystem::remove_all(tempHome, ec);
  if (ec) {
    std::cerr << "warning: cleanup failed for " << tempHome << ": "
              << ec.message() << std::endl;
  }

  return result;
}

} // namespace firmius::audits
