#include "tools/ProcessTool.hpp"

#include "AgentRegistry.hpp"
#include "IAgent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "environment/PermissionSuggestionEngine.hpp"
#include "harness/Harness.hpp"
#include "utils/FSUtil.hpp"
#include "utils/SpillIfLarge.hpp"
#include "utils/TerminalUtil.hpp"
#include <chrono>
#include <cctype>
#include <algorithm>
#include <iomanip>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <thread>

namespace firmius::core {
using namespace firmius::shared;

namespace {

// Cwd gate: each Process invocation passes its resolved cwd through the
// process.cwd policy category. This catches the "agent cd's into a
// different project / /tmp / etc." case that command-allowlists alone
// can't see — the cwd is part of the threat model just as much as the
// command string is.
inline void gateProcessCwd(shared::ToolContext &ctx, const std::string &cwd) {
  if (cwd.empty()) return;
  PolicyRequest req;
  req.category = kCatProcessCwd;
  req.cwd = cwd;
  req.toolName = "Process";
  auto eval = Harness::instance().policyEngine().evaluate(req);
  if (eval.decision == PolicyDecision::Allow) return;
  if (eval.decision == PolicyDecision::Deny) {
    throw std::runtime_error("Process cwd denied by policy: " + cwd);
  }
  shared::PermissionEscalationRequest esc;
  const auto &actx = ctx.agent.getContext();
  esc.threadId = actx.history ? actx.history->threadId : "";
  esc.agentId = actx.identity.id;
  esc.toolName = "Process";
  esc.requestType = shared::PermissionRequestType::Read;
  esc.title = "Allow this cwd?";
  esc.message = "The agent wants to run a process from " + cwd;
  esc.severity = shared::CommandSeverity::LOW;
  esc.allowAlways = true;
  esc.category = req.category;
  esc.cwd = cwd;
  shared::CommandIntent dummy;
  auto suggestions = PermissionSuggestionEngine::generate(req, dummy);
  auto response = Harness::instance().requestPermissionEscalationWithSuggestions(
      std::move(esc), std::move(suggestions));
  if (response == shared::PermissionResponse::Deny) {
    throw std::runtime_error("Process cwd denied: " + cwd);
  }
}

// Token-waste pass 2: thresholds for inline output before we tail / spill.
// Process stdout/stderr commonly hit megabytes (find, grep -r, build logs);
// keeping them inline blows the LLM context. We keep at most ~4 KB per
// stream by default, and spill the full output to /tmp when it crosses
// 64 KB.
constexpr std::size_t kProcessTailBytes = 4 * 1024;
constexpr std::size_t kProcessSpillThresholdBytes = 64 * 1024;

shared::ToolResult failWithStructuredData(const rapidjson::Document &d,
                                          const std::string &error) {
  shared::ToolResult result = shared::ToolResult::fail(error);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  result.data = buffer.GetString();
  return result;
}

rapidjson::Value makeString(const std::string &value,
                            rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value result;
  result.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), alloc);
  return result;
}

// Explicit slicing for Process.Output: honors `from_offset`, `max_bytes`,
// and `tail_lines` exactly as documented (the model uses these to do its
// own pagination). Kept as a separate helper since it has different
// semantics than the cursor-driven Status/Wait path.
std::string sliceOutput(const std::string &data, int fromOffset, int maxBytes,
                        int tailLines) {
  size_t start = fromOffset > 0
                     ? std::min(static_cast<size_t>(fromOffset), data.size())
                     : 0;
  if (tailLines > 0 && !data.empty()) {
    size_t pos = data.size();
    int seen = 0;
    while (pos > 0 && seen < tailLines) {
      --pos;
      if (data[pos] == '\n') {
        ++seen;
        if (seen == tailLines) {
          if (pos + 1 < data.size()) {
            ++pos;
          }
          break;
        }
      }
    }
    if (seen < tailLines) {
      pos = 0;
    }
    start = std::max(start, pos);
  }
  size_t count = data.size() - start;
  if (maxBytes > 0) {
    count = std::min(count, static_cast<size_t>(maxBytes));
  }
  return data.substr(start, count);
}

// Token-waste pass 2: take the bytes of `data` that appear at index >= cursor,
// then apply tail-by-default to the result. Returns the slice plus the new
// cursor that the caller can persist.
struct StreamSlice {
  std::string text;
  std::size_t newCursor = 0;
  bool hadNewBytes = false;
};

StreamSlice sliceSinceCursor(const std::string &data, std::size_t cursor,
                             std::size_t tailBytes) {
  StreamSlice s;
  s.newCursor = data.size();
  if (cursor >= data.size()) {
    return s;  // no new bytes
  }
  s.hadNewBytes = true;
  std::string fresh = data.substr(cursor);
  if (fresh.size() <= tailBytes) {
    s.text = std::move(fresh);
    return s;
  }
  // Trim to last tailBytes at a line boundary so we don't split a line.
  std::size_t start = fresh.size() - tailBytes;
  while (start < fresh.size() && fresh[start] != '\n') ++start;
  if (start < fresh.size()) ++start;
  if (start >= fresh.size()) start = fresh.size() - tailBytes;
  s.text = fresh.substr(start);
  return s;
}

// Build the prose-first single-line summary used by every Process result.
// Examples:
//   "Exited 0 in 12 ms. stdout: 142 B (4 lines). stderr: empty."
//   "Still running (1.2s elapsed). stdout: 312 KB total / 4823 lines (showing last 4 KB)."
std::string buildProcessProse(const shared::ProcessSnapshot &snap,
                              const std::string &finishReason,
                              std::size_t stdoutTotal,
                              std::size_t stderrTotal,
                              const std::string &stdoutSpillRef,
                              const std::string &stderrSpillRef,
                              std::size_t stdoutTailLen,
                              std::size_t stderrTailLen) {
  std::ostringstream s;
  if (!finishReason.empty() && finishReason != "Natural") {
    // Token-waste pass 2: explicit finish reason ("Timeout", "Interrupted")
    // takes precedence over the live running flag — at timeout the
    // process is technically still alive, but the agent needs to know
    // why we stopped waiting.
    s << finishReason << " after " << snap.elapsedMs << " ms (exit "
      << snap.exitCode << ")";
  } else if (snap.running) {
    s << "Still running (" << snap.elapsedMs << " ms elapsed)";
  } else {
    s << "Exited " << snap.exitCode << " in " << snap.elapsedMs << " ms";
  }

  auto fmtBytes = [](std::size_t n) {
    std::ostringstream o;
    if (n < 1024) {
      o << n << " B";
    } else if (n < 1024ull * 1024) {
      o.precision(1);
      o << std::fixed << (static_cast<double>(n) / 1024.0) << " KB";
    } else {
      o.precision(2);
      o << std::fixed << (static_cast<double>(n) / (1024.0 * 1024.0)) << " MB";
    }
    return o.str();
  };

  auto streamSummary = [&](const char *label, std::size_t total,
                           std::size_t tailLen, const std::string &spillRef) {
    s << ". " << label << ": ";
    if (total == 0) {
      s << "empty";
      return;
    }
    s << fmtBytes(total);
    if (!spillRef.empty()) {
      s << " total spilled to " << spillRef
        << " (showing last " << fmtBytes(tailLen) << ")";
    } else if (tailLen < total) {
      s << " (showing last " << fmtBytes(tailLen) << ")";
    }
  };

  streamSummary("stdout", stdoutTotal, stdoutTailLen, stdoutSpillRef);
  streamSummary("stderr", stderrTotal, stderrTailLen, stderrSpillRef);
  s << ".";
  return s.str();
}

// Token-waste pass 2: replaces the old addSnapshotFields().
// Builds a prose-first result doc with optional tail/spill on the output
// streams. When `useCursor` is true, only emits stdout/stderr bytes since
// the per-agent cursor for this process_id; the cursor is advanced
// in-place so consecutive polls do NOT re-echo the entire buffer.
//
// Result shape:
//   {
//     "result":         "Exited 0 in 12 ms. stdout: 4.2 KB (showing last 4 KB). stderr: empty.",
//     "process_id":     "...",
//     "running":        bool,
//     "exit_code":      int,
//     "duration_ms":    int,
//     "stdout":         "<tail or full>",        // omitted when empty
//     "stderr":         "<tail or full>",        // omitted when empty
//     "stdout_bytes":   uint,                    // total accumulated bytes
//     "stderr_bytes":   uint,                    // total accumulated bytes
//     "stdout_ref":     "/tmp/...",              // only when spilled
//     "stderr_ref":     "/tmp/..."               // only when spilled
//   }
//
// The legacy duplicated fields (`isRunning`, `exitCode`, `system_id` for
// non-spawn paths) are gone. Spawn-time `system_id` is still emitted by
// executeSpawn explicitly because it's useful at handoff.
void buildProcessResultDoc(rapidjson::Document &doc,
                           const shared::ProcessSnapshot &snapshot,
                           const std::string &processId,
                           const std::string &finishReason,
                           bool useCursor,
                           shared::ToolContext &ctx) {
  auto &a = doc.GetAllocator();

  std::size_t stdoutCursor = 0;
  std::size_t stderrCursor = 0;
  if (useCursor) {
    auto &state = ctx.agent.getMutableContext().state;
    auto itOut = state.processStdoutCursor.find(processId);
    if (itOut != state.processStdoutCursor.end()) stdoutCursor = itOut->second;
    auto itErr = state.processStderrCursor.find(processId);
    if (itErr != state.processStderrCursor.end()) stderrCursor = itErr->second;
  }

  auto stdoutSlice = sliceSinceCursor(snapshot.stdoutData, stdoutCursor,
                                      kProcessTailBytes);
  auto stderrSlice = sliceSinceCursor(snapshot.stderrData, stderrCursor,
                                      kProcessTailBytes);

  // Spill to /tmp when the cumulative stream crossed the threshold. We
  // spill on cumulative size (not slice size) so the user always has the
  // full log on disk for grep/inspection regardless of when they polled.
  shared::utils::SpillResult stdoutSpill;
  shared::utils::SpillResult stderrSpill;
  if (snapshot.stdoutData.size() > kProcessSpillThresholdBytes) {
    stdoutSpill = shared::utils::spillIfLarge(
        snapshot.stdoutData, kProcessSpillThresholdBytes,
        "firmius_proc_stdout_" + processId, kProcessTailBytes);
  }
  if (snapshot.stderrData.size() > kProcessSpillThresholdBytes) {
    stderrSpill = shared::utils::spillIfLarge(
        snapshot.stderrData, kProcessSpillThresholdBytes,
        "firmius_proc_stderr_" + processId, kProcessTailBytes);
  }

  // Build prose. Use cumulative stream sizes (the model wants to know the
  // process produced 312 KB even though we are showing 4 KB).
  const std::string prose = buildProcessProse(
      snapshot, finishReason,
      snapshot.stdoutData.size(), snapshot.stderrData.size(),
      stdoutSpill.refPath, stderrSpill.refPath,
      stdoutSlice.text.size(), stderrSlice.text.size());
  doc.AddMember("result", makeString(prose, a), a);
  doc.AddMember("process_id", makeString(processId, a), a);
  doc.AddMember("running", snapshot.running, a);
  doc.AddMember("exit_code", snapshot.exitCode, a);
  doc.AddMember("duration_ms", snapshot.elapsedMs, a);
  if (!stdoutSlice.text.empty()) {
    doc.AddMember("stdout", makeString(stdoutSlice.text, a), a);
  }
  if (!stderrSlice.text.empty()) {
    doc.AddMember("stderr", makeString(stderrSlice.text, a), a);
  }
  doc.AddMember("stdout_bytes",
                static_cast<uint64_t>(snapshot.stdoutData.size()), a);
  doc.AddMember("stderr_bytes",
                static_cast<uint64_t>(snapshot.stderrData.size()), a);
  if (!stdoutSpill.refPath.empty()) {
    doc.AddMember("stdout_ref", makeString(stdoutSpill.refPath, a), a);
  }
  if (!stderrSpill.refPath.empty()) {
    doc.AddMember("stderr_ref", makeString(stderrSpill.refPath, a), a);
  }

  if (useCursor) {
    auto &state = ctx.agent.getMutableContext().state;
    state.processStdoutCursor[processId] = stdoutSlice.newCursor;
    state.processStderrCursor[processId] = stderrSlice.newCursor;
  }
}

shared::ToolResult executeExecute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string command;
  if (input.HasMember("command") && input["command"].IsString()) {
    command = input["command"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: command");
  }

  std::string cwd;
  if (input.HasMember("cwd") && input["cwd"].IsString()) {
    cwd = input["cwd"].GetString();
  }

  int timeout_ms = 15000;
  if (input.HasMember("timeout_ms") && input["timeout_ms"].IsInt()) {
    timeout_ms = input["timeout_ms"].GetInt();
  }

  std::string processId;
  shared::ProcessSnapshot snap;
  try {
    std::string normalizedCommand;
    normalizedCommand.reserve(command.size());
    bool previousWasSpace = false;
    for (char ch : command) {
      if (std::isspace(static_cast<unsigned char>(ch))) {
        if (!previousWasSpace) {
          normalizedCommand.push_back(' ');
          previousWasSpace = true;
        }
      } else {
        normalizedCommand.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        previousWasSpace = false;
      }
    }
    if (!normalizedCommand.empty() && normalizedCommand.front() == ' ') normalizedCommand.erase(normalizedCommand.begin());
    if (!normalizedCommand.empty() && normalizedCommand.back() == ' ') normalizedCommand.pop_back();

    const bool mentionsApplyPatch =
        normalizedCommand == "apply_patch" ||
        normalizedCommand.rfind("apply_patch ", 0) == 0 ||
        normalizedCommand.find(" apply_patch ") != std::string::npos ||
        normalizedCommand.find("| apply_patch") != std::string::npos ||
        normalizedCommand.find("&& apply_patch") != std::string::npos ||
        normalizedCommand.find("|| apply_patch") != std::string::npos;
    if (mentionsApplyPatch) {
      return shared::ToolResult::fail("Foreign tool conflict: 'apply_patch' is not available in Firmius and must not be run via Process.execute. Use Files (Read) + Edit (patch) instead.");
    }

    std::string effectiveCwd = cwd.empty() ? ctx.agent.getContext().environment.cwd : cwd;
    effectiveCwd = ctx.agent.getEnvironment()->getWorkspace().resolvePath(effectiveCwd);

    gateProcessCwd(ctx, effectiveCwd);
    ctx.agent.getPermissions()->validatePathAccess(effectiveCwd, firmius::shared::AccessMode::READ);
    auto intent = ctx.agent.getPermissions()->getIntentAnalyzer().analyze(command, effectiveCwd);
    auto approval = ctx.agent.getPermissions()->requestCommandApproval(command, intent, "Process");
    if (approval == PermissionResponse::Deny) {
      return shared::ToolResult::fail("Command execution denied: " + command);
    }

    processId = ctx.agent.getEnvironment()->getProcessManager().spawnProcess(command, ctx.currentToolCallId, effectiveCwd, {}, true);
    ctx.agent.getEnvironment()->getProcessManager().addBlockingProcessId(processId);

    auto start = std::chrono::steady_clock::now();
    auto sleepDuration = std::chrono::milliseconds(1);
    const auto maxSleep = std::chrono::milliseconds(20);

    while (true) {
      if (ctx.cancelRequested()) {
        snap = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
        ctx.agent.getEnvironment()->getProcessManager().killProcess(processId);
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);

        rapidjson::Document doc; doc.SetObject();
        snap.exitCode = -2;
        buildProcessResultDoc(doc, snap, processId, "Interrupted",
                              /*useCursor=*/false, ctx);
        return shared::ToolResult::ok(doc, processId);
      }

      snap = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
      if (!snap.running) break;

      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
      if (elapsed > timeout_ms) {
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
        rapidjson::Document doc; doc.SetObject();
        snap.exitCode = -1;
        buildProcessResultDoc(doc, snap, processId, "Timeout",
                              /*useCursor=*/false, ctx);
        // Background-continuation hint folded into the prose `result`
        // string by buildProcessProse via the "Timeout" finish reason;
        // we keep an explicit `note` only for backward-compat consumers
        // that scrape this hint.
        doc.AddMember(
            "note",
            rapidjson::Value(
                "Still running in the background. Use Process.status to "
                "check current output or Process.wait to wait for completion.",
                doc.GetAllocator())
                .Move(),
            doc.GetAllocator());
        auto result = shared::ToolResult::ok(doc, processId);
        result.is_background = true;
        return result;
      }

      if (!ctx.waitFor(sleepDuration)) {
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
        rapidjson::Document doc; doc.SetObject();
        snap.exitCode = -2;
        buildProcessResultDoc(doc, snap, processId, "Interrupted",
                              /*useCursor=*/false, ctx);
        return shared::ToolResult::ok(doc, processId);
      }
      if (sleepDuration < maxSleep) sleepDuration = std::min(maxSleep, sleepDuration * 2);
    }

    ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
    rapidjson::Document doc; doc.SetObject();
    buildProcessResultDoc(doc, snap, processId, "Natural",
                          /*useCursor=*/false, ctx);

    if (snap.exitCode != 0) {
      return failWithStructuredData(doc, "Command exited with non-zero exit code: " + std::to_string(snap.exitCode));
    }
    return shared::ToolResult::ok(doc, processId);
  } catch (const std::exception &e) {
    if (!processId.empty()) ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeSpawn(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string command;
  if (input.HasMember("command") && input["command"].IsString()) {
    command = input["command"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: command");
  }

  std::string cwd;
  if (input.HasMember("cwd") && input["cwd"].IsString()) {
    cwd = input["cwd"].GetString();
  }

  try {
    std::string effectiveCwd = cwd.empty() ? ctx.agent.getContext().environment.cwd : cwd;
    effectiveCwd = ctx.agent.getEnvironment()->getWorkspace().resolvePath(effectiveCwd);

    gateProcessCwd(ctx, effectiveCwd);
    ctx.agent.getPermissions()->validatePathAccess(effectiveCwd, firmius::shared::AccessMode::READ);
    auto intent = ctx.agent.getPermissions()->getIntentAnalyzer().analyze(command, effectiveCwd);
    auto approval = ctx.agent.getPermissions()->requestCommandApproval(command, intent, "Process");
    if (approval == PermissionResponse::Deny) {
      return shared::ToolResult::fail("Command execution denied: " + command);
    }

    std::string processId = ctx.agent.getEnvironment()->getProcessManager().spawnProcess(command, ctx.currentToolCallId, effectiveCwd, {});
    auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);

    // Token-waste pass 2: Spawn returns just the prose + ids needed to
    // refer back to the process; no stdout/stderr dump (it's a fresh
    // spawn, no output yet).
    rapidjson::Document doc; doc.SetObject();
    auto &a = doc.GetAllocator();
    doc.AddMember(
        "result",
        makeString("Spawned process " + processId + ".", a), a);
    doc.AddMember("process_id", makeString(processId, a), a);
    doc.AddMember("system_id", makeString(snapshot.systemId, a), a);
    return shared::ToolResult::ok(doc, processId);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeStatus(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string process_id;
  if (input.HasMember("process_id") && input["process_id"].IsString()) {
    process_id = input["process_id"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: process_id");
  }

  try {
    if (AgentRegistry::instance().getAgent(process_id)) {
      return shared::ToolResult::fail("ID '" + process_id + "' belongs to a subagent, not a process. Use Delegate.wait (or Delegate.stop) for agent IDs.");
    }

    auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
    rapidjson::Document doc; doc.SetObject();
    // Token-waste pass 2: cursor-aware. Successive Status calls only
    // return stdout/stderr that arrived since the last Status/Wait.
    buildProcessResultDoc(doc, snapshot, process_id, /*finishReason=*/"",
                          /*useCursor=*/true, ctx);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception& e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeWait(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string process_id;
  if (input.HasMember("process_id") && input["process_id"].IsString()) {
    process_id = input["process_id"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: process_id");
  }

  std::string pattern;
  if (input.HasMember("pattern") && input["pattern"].IsString()) {
    pattern = input["pattern"].GetString();
  }

  int timeout_ms = 30000;
  if (input.HasMember("timeout_ms") && input["timeout_ms"].IsInt()) {
    timeout_ms = input["timeout_ms"].GetInt();
  }

  try {
    auto startTime = std::chrono::steady_clock::now();
    while (true) {
      if (ctx.cancelRequested()) return shared::ToolResult::fail("Interrupted");
      auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
      bool patternFound = false;
      if (!pattern.empty()) {
        if (snapshot.stdoutData.find(pattern) != std::string::npos || snapshot.stderrData.find(pattern) != std::string::npos) patternFound = true;
      }
      if (!snapshot.running || patternFound) {
        rapidjson::Document doc; doc.SetObject();
        // Wait is also cursor-aware: when the agent calls Wait after a
        // Status, only NEW output is reported.
        buildProcessResultDoc(doc, snapshot, process_id, /*finishReason=*/"",
                              /*useCursor=*/true, ctx);
        // Token-waste pass 2: pattern_found is always emitted on Wait
        // returns — true when the pattern triggered, false when the
        // process exited first. Stable contract beats conditional
        // emission for a small bool.
        doc.AddMember("pattern_found", patternFound, doc.GetAllocator());
        return shared::ToolResult::ok(doc);
      }
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
      if (elapsed >= timeout_ms) return shared::ToolResult::fail("Timeout reached while waiting for process " + process_id);
      if (!ctx.waitFor(std::chrono::milliseconds(25))) return shared::ToolResult::fail("Interrupted");
    }
  } catch (const std::exception& e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeInput(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string process_id;
  if (input.HasMember("process_id") && input["process_id"].IsString()) {
    process_id = input["process_id"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: process_id");
  }

  std::string text;
  if (input.HasMember("input") && input["input"].IsString()) {
    text = input["input"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: input");
  }

  try {
    std::string translated = shared::TerminalUtil::translate(text);
    int linesSent = 0;
    int charsSent = 0;
    size_t last = 0;
    size_t next = 0;
    while ((next = translated.find('\n', last)) != std::string::npos) {
      std::string part = translated.substr(last, next - last + 1);
      ctx.agent.getEnvironment()->getProcessManager().writeToProcess(process_id, part);
      linesSent++;
      charsSent += static_cast<int>(part.size());
      auto start = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000)) {
        if (ctx.cancelRequested()) return shared::ToolResult::fail("Interrupted");
        if (!ctx.waitFor(std::chrono::milliseconds(10))) return shared::ToolResult::fail("Interrupted");
      }
      last = next + 1;
    }
    if (last < translated.size()) {
      std::string remaining = translated.substr(last);
      ctx.agent.getEnvironment()->getProcessManager().writeToProcess(process_id, remaining);
      charsSent += static_cast<int>(remaining.size());
    }
    // Token-waste pass 2: drop the full `sent` echo (the model just sent
    // it). Return prose + counts only.
    rapidjson::Document doc; doc.SetObject();
    auto &a = doc.GetAllocator();
    std::ostringstream prose;
    prose << "Sent " << charsSent << " char" << (charsSent == 1 ? "" : "s")
          << " (" << linesSent << " line" << (linesSent == 1 ? "" : "s")
          << ") to process " << process_id << ".";
    doc.AddMember("result", makeString(prose.str(), a), a);
    doc.AddMember("chars", charsSent, a);
    doc.AddMember("lines", linesSent, a);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception& e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeKill(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string process_id;
  if (input.HasMember("process_id") && input["process_id"].IsString()) {
    process_id = input["process_id"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: process_id");
  }

  try {
    auto before = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
    ctx.agent.getEnvironment()->getProcessManager().killProcess(process_id);
    auto after = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
    auto start = std::chrono::steady_clock::now();
    while (after.running &&
           std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000)) {
      if (!ctx.waitFor(std::chrono::milliseconds(25))) {
        break;
      }
      after = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
    }
    rapidjson::Document doc; doc.SetObject();
    auto &a = doc.GetAllocator();
    // Token-waste pass 2: prose-first kill summary. We retain `running`
    // and `exit_code` from the post-kill snapshot via buildProcessResultDoc
    // for consistency with Status, but the prose makes the actionable
    // bit ("killed" vs "was already finished") trivial to read.
    const bool wasRunning = before.running;
    const bool nowDead = wasRunning && !after.running;
    buildProcessResultDoc(doc, after, process_id, /*finishReason=*/"",
                          /*useCursor=*/false, ctx);
    // Overwrite the `result` prose with a kill-specific phrasing.
    std::string killProse;
    if (nowDead) {
      killProse = "Killed process " + process_id + " (was running).";
    } else if (wasRunning) {
      killProse = "Sent kill to process " + process_id +
                  " but it is still alive after 1s grace.";
    } else {
      killProse = "Process " + process_id + " was not running; nothing to kill.";
    }
    doc["result"].SetString(killProse.c_str(),
                            static_cast<rapidjson::SizeType>(killProse.size()),
                            a);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception& e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeOutput(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string process_id;
  if (input.HasMember("process_id") && input["process_id"].IsString()) {
    process_id = input["process_id"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: process_id");
  }

  int fromOffset = 0;
  if (input.HasMember("from_offset") && input["from_offset"].IsInt()) {
    fromOffset = input["from_offset"].GetInt();
  }
  int maxBytes = 0;
  if (input.HasMember("max_bytes") && input["max_bytes"].IsInt()) {
    maxBytes = input["max_bytes"].GetInt();
  }
  int tailLines = 0;
  if (input.HasMember("tail_lines") && input["tail_lines"].IsInt()) {
    tailLines = input["tail_lines"].GetInt();
  }
  std::string stream = "combined";
  if (input.HasMember("stream") && input["stream"].IsString()) {
    stream = input["stream"].GetString();
  }

  try {
    auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
    const std::string combined = snapshot.stdoutData + snapshot.stderrData;
    const std::string *source = &combined;
    if (stream == "stdout") {
      source = &snapshot.stdoutData;
    } else if (stream == "stderr") {
      source = &snapshot.stderrData;
    } else if (stream != "combined") {
      return shared::ToolResult::fail("Process.Output stream must be stdout, stderr, or combined");
    }

    std::string output = sliceOutput(*source, fromOffset, maxBytes, tailLines);
    rapidjson::Document doc; doc.SetObject(); auto &a = doc.GetAllocator();
    // Token-waste pass 2: drop isRunning/system_id echoes; keep the
    // explicit slicing contract (from_offset/next_offset/output) intact
    // because the model uses these to paginate Process.Output calls.
    std::ostringstream prose;
    prose << "Read " << output.size() << " B of " << stream
          << " from process " << process_id
          << " (offset " << fromOffset << " → " << source->size() << ")";
    if (snapshot.running) prose << ", still running";
    prose << ".";
    doc.AddMember("result", makeString(prose.str(), a), a);
    doc.AddMember("process_id", makeString(process_id, a), a);
    doc.AddMember("running", snapshot.running, a);
    doc.AddMember("stream", makeString(stream, a), a);
    doc.AddMember("output", makeString(output, a), a);
    doc.AddMember("from_offset", fromOffset, a);
    doc.AddMember("next_offset", static_cast<uint64_t>(source->size()), a);
    doc.AddMember("stdout_bytes", static_cast<uint64_t>(snapshot.stdoutData.size()), a);
    doc.AddMember("stderr_bytes", static_cast<uint64_t>(snapshot.stderrData.size()), a);
    if (maxBytes > 0 && output.size() == static_cast<size_t>(maxBytes)) {
      doc.AddMember("truncated", true, a);
    }
    return shared::ToolResult::ok(doc);
  } catch (const std::exception& e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeList(const rapidjson::Value &, shared::ToolContext &ctx) {
  rapidjson::Document doc; doc.SetObject(); auto &a = doc.GetAllocator();
  rapidjson::Value processes(rapidjson::kArrayType);
  std::ostringstream prose;
  std::size_t count = 0;
  std::size_t running = 0;
  for (const auto &processId : ctx.agent.getEnvironment()->getProcessManager().getProcessIds()) {
    try {
      auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
      // Token-waste pass 2: per-process entry is now the minimum identifying
      // set: id, running flag, exit code, duration, total bytes. Dropped
      // duplicated camelCase fields and system_id from the list view.
      rapidjson::Value process(rapidjson::kObjectType);
      process.AddMember("process_id", makeString(processId, a), a);
      process.AddMember("running", snapshot.running, a);
      process.AddMember("exit_code", snapshot.exitCode, a);
      process.AddMember("duration_ms", snapshot.elapsedMs, a);
      process.AddMember("stdout_bytes",
                        static_cast<uint64_t>(snapshot.stdoutData.size()), a);
      process.AddMember("stderr_bytes",
                        static_cast<uint64_t>(snapshot.stderrData.size()), a);
      processes.PushBack(process, a);
      ++count;
      if (snapshot.running) ++running;
    } catch (...) {
    }
  }
  if (count == 0) {
    prose << "No managed processes.";
  } else {
    prose << count << " managed process" << (count == 1 ? "" : "es") << " ("
          << running << " running, " << (count - running) << " finished).";
  }
  doc.AddMember("result", makeString(prose.str(), a), a);
  doc.AddMember("processes", processes, a);
  return shared::ToolResult::ok(doc);
}

} // namespace

shared::ToolMetadata ProcessTool::getMetadata() const {
  return {"Process",
          R"(Process operations for running commands and interacting with live child processes.

USAGE GUIDANCE:
- Use Execute for a blocking command when you want a final exit code/result in one step.
- Use Spawn for long-running commands, then follow with Status/Output/Wait/Input/Kill as needed.
- If you Spawn a process, you are expected to eventually Wait, Kill, or otherwise settle it; do not leave ghost processes behind.
- Never use Process as an editing tunnel for writing repository files; use Edit/EditWrite/EditReplace/EditRange for file modifications.
- Prefer narrow verification commands that produce binary evidence (exit code / exact output).

ACTIONS:
- Execute: run a command and wait for completion.
- Spawn: start a background process and get a process_id.
- Status: inspect running/completed state.
- Wait: wait for completion or a matching output condition.
- Input: send stdin to a live process.
- Output: read stdout/stderr slices from a live process.
- List: inspect known managed processes.
- Kill: terminate a managed process.
)",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> ProcessTool::getSchema() const {
  return shared::zObject({
      {"action",
       shared::zEnum({"Execute", "Spawn", "Status", "Wait", "Input", "Output", "List", "Kill"})
           ->describe(
               "Which process operation to perform.\n\n"
               "- Execute: run a command to completion\n"
               "- Spawn: start a background process\n"
               "- Status: inspect a process\n"
               "- Wait: wait for completion and/or output conditions\n"
               "- Input: send stdin to a live process\n"
               "- Output: read buffered stdout/stderr\n"
               "- List: list managed processes\n"
               "- Kill: terminate a managed process")},
      {"command",
       shared::zString()->setOptional()->describe(
           "Shell command to run for Execute or Spawn.\n\n"
           "Required for Execute/Spawn. Use workspace-safe verification commands when possible.")},
      {"process_id",
       shared::zString()->setOptional()->describe(
           "Managed process identifier returned by Spawn.\n\n"
           "Required for Status/Wait/Input/Output/Kill.")},
      {"input",
       shared::zString()->setOptional()->describe(
           "Text to send to stdin for Input action.")},
      {"pattern",
       shared::zString()->setOptional()->describe(
           "Optional regex/text pattern used by Wait to stop when matching output appears.")},
      {"stream",
       shared::zString()->setOptional()->describe(
           "Which output stream to inspect for Output action. Typically 'stdout' or 'stderr'.")},
      {"cwd",
       shared::zString()->setOptional()->describe(
           "Working directory for Execute/Spawn. Prefer workspace-relative directories.")},
      {"timeout_ms",
       shared::zInteger()->setOptional()->describe(
           "Timeout in milliseconds for Execute/Wait. Use to bound verification steps.")},
      {"from_offset",
       shared::zInteger()->setOptional()->describe(
           "Byte offset for Output reads so you can continue from a prior cursor.")},
      {"max_bytes",
       shared::zInteger()->setOptional()->describe(
           "Maximum bytes to return for Output. Use to keep large logs bounded.")},
      {"tail_lines",
       shared::zInteger()->setOptional()->describe(
           "Optional number of trailing lines to return for Output/List-style inspection.")},
  });
}

shared::ToolResult ProcessTool::execute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  if (!input.IsObject()) {
    return shared::ToolResult::fail("Process input must be an object");
  }
  std::string action;
  if (input.HasMember("action") && input["action"].IsString()) {
    action = input["action"].GetString();
  } else if (input.HasMember("command") && input["command"].IsString()) {
    action = "Spawn";
  } else if (input.HasMember("input") && input["input"].IsString()) {
    action = "Input";
  } else if (input.HasMember("process_id") && input["process_id"].IsString() &&
             (input.HasMember("pattern") || input.HasMember("timeout_ms"))) {
    action = "Wait";
  } else if (input.HasMember("process_id") && input["process_id"].IsString()) {
    action = "Status";
  } else {
    return shared::ToolResult::fail("Process.action must be a string (Execute, Spawn, Status, Wait, Input, Output, List, or Kill)");
  }
  if (action == "Execute") return executeExecute(input, ctx);
  if (action == "Spawn") return executeSpawn(input, ctx);
  if (action == "Status") return executeStatus(input, ctx);
  if (action == "Wait") return executeWait(input, ctx);
  if (action == "Input") return executeInput(input, ctx);
  if (action == "Output") return executeOutput(input, ctx);
  if (action == "List") return executeList(input, ctx);
  if (action == "Kill") return executeKill(input, ctx);
  return shared::ToolResult::fail("Process.action must be Execute, Spawn, Status, Wait, Input, Output, List, or Kill");
}

} // namespace firmius::core
