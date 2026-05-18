#include "tools/PythonExecuteTool.hpp"

#include "agents/Agent.hpp"
#include "utils/SpillIfLarge.hpp"
#include <chrono>
#include <filesystem>
#include <map>
#include <rapidjson/document.h>
#include <sstream>

namespace firmius::core {
using namespace firmius::shared;

namespace {

// Token-waste pass 2: tail/spill thresholds match Process tool defaults.
constexpr std::size_t kPyTailBytes = 4 * 1024;
constexpr std::size_t kPySpillThresholdBytes = 64 * 1024;

rapidjson::Value pyMakeString(const std::string &value,
                              rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value v;
  v.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()),
              alloc);
  return v;
}

std::string pyFmtBytes(std::size_t n) {
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
}

// Build the prose-first Python result doc with tail+spill applied to
// stdout/stderr. Drops the `command` echo (model didn't send it; we
// synthesised the wrapper) and the `venv` echo (was in input). Drops
// duplicate `process_id` for the synchronous one-shot case; keeping
// only the exit code, duration, and tailed streams.
//
// Result shape:
//   {
//     "result":      "Exited 0 in 38 ms. stdout: 142 B. stderr: empty.",
//     "exit_code":   int,
//     "duration_ms": int,
//     "stdout":      "<tail or full>",   // omitted when empty
//     "stderr":      "<tail or full>",   // omitted when empty
//     "stdout_bytes": uint,              // total accumulated
//     "stderr_bytes": uint,
//     "stdout_ref":   "/tmp/...",        // only when spilled
//     "stderr_ref":   "/tmp/..."
//   }
void buildPyResultDoc(rapidjson::Document &doc,
                      const shared::ProcessSnapshot &snap,
                      const std::string &finishReason,
                      const std::string &processId) {
  auto &a = doc.GetAllocator();

  shared::utils::SpillResult outSpill;
  shared::utils::SpillResult errSpill;
  if (snap.stdoutData.size() > kPySpillThresholdBytes) {
    outSpill = shared::utils::spillIfLarge(
        snap.stdoutData, kPySpillThresholdBytes,
        "firmius_py_stdout_" + processId, kPyTailBytes);
  }
  if (snap.stderrData.size() > kPySpillThresholdBytes) {
    errSpill = shared::utils::spillIfLarge(
        snap.stderrData, kPySpillThresholdBytes,
        "firmius_py_stderr_" + processId, kPyTailBytes);
  }

  // Compute the inline tail texts (independent of spill — even non-spilled
  // streams get tailed if they are bigger than kPyTailBytes).
  auto tailText = [](const std::string &full) {
    if (full.size() <= kPyTailBytes) return full;
    std::size_t start = full.size() - kPyTailBytes;
    while (start < full.size() && full[start] != '\n') ++start;
    if (start < full.size()) ++start;
    if (start >= full.size()) start = full.size() - kPyTailBytes;
    return full.substr(start);
  };
  const std::string outTail = tailText(snap.stdoutData);
  const std::string errTail = tailText(snap.stderrData);

  // Prose summary.
  std::ostringstream prose;
  if (!finishReason.empty() && finishReason != "Natural") {
    prose << finishReason << " after " << snap.elapsedMs << " ms (exit "
          << snap.exitCode << ")";
  } else {
    prose << "Exited " << snap.exitCode << " in " << snap.elapsedMs << " ms";
  }
  auto streamBit = [&](const char *label, const std::string &full,
                       const std::string &tail,
                       const shared::utils::SpillResult &spill) {
    prose << ". " << label << ": ";
    if (full.empty()) {
      prose << "empty";
      return;
    }
    prose << pyFmtBytes(full.size());
    if (!spill.refPath.empty()) {
      prose << " total spilled to " << spill.refPath
            << " (showing last " << pyFmtBytes(tail.size()) << ")";
    } else if (tail.size() < full.size()) {
      prose << " (showing last " << pyFmtBytes(tail.size()) << ")";
    }
  };
  streamBit("stdout", snap.stdoutData, outTail, outSpill);
  streamBit("stderr", snap.stderrData, errTail, errSpill);
  prose << ".";

  doc.AddMember("result", pyMakeString(prose.str(), a), a);
  doc.AddMember("exit_code", snap.exitCode, a);
  doc.AddMember("duration_ms", snap.elapsedMs, a);
  if (!outTail.empty()) {
    doc.AddMember("stdout", pyMakeString(outTail, a), a);
  }
  if (!errTail.empty()) {
    doc.AddMember("stderr", pyMakeString(errTail, a), a);
  }
  doc.AddMember("stdout_bytes",
                static_cast<uint64_t>(snap.stdoutData.size()), a);
  doc.AddMember("stderr_bytes",
                static_cast<uint64_t>(snap.stderrData.size()), a);
  if (!outSpill.refPath.empty()) {
    doc.AddMember("stdout_ref", pyMakeString(outSpill.refPath, a), a);
  }
  if (!errSpill.refPath.empty()) {
    doc.AddMember("stderr_ref", pyMakeString(errSpill.refPath, a), a);
  }
}

}  // namespace

shared::ToolMetadata PythonExecuteTool::getMetadata() const {
  return {"Python",
          R"(Execute Python code on the host as a real process.

USAGE GUIDANCE:
- Use this only when Python is the correct execution surface, not as a shortcut for editing files.
- Do NOT use Python to write repository files; use the Edit-family tools for all file modifications.
- Good uses: structured data inspection, one-off calculations, parsing/transforms for analysis, probing environments that already have the needed interpreter/packages.
- If a project virtualenv should be used, pass venv so the tool runs that interpreter.

EXECUTION MODEL:
- The tool runs Python via a managed process and returns stdout/stderr/exit_code.
- It is subject to normal command approval / process permissions.
)",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> PythonExecuteTool::getSchema() const {
  return shared::zObject(
             {{"code", shared::zString()->describe(
                            "Python source code to execute.\n\n"
                            "Use for analysis/execution, not file editing. The code is executed as a managed host process and its stdout/stderr are returned.")},
              {"venv", shared::zString()
                           ->describe(
                               "Optional path to a Python virtual environment whose interpreter should be used.\n\n"
                               "Pass the virtualenv root (the tool resolves '<venv>/bin/python'). The path must be readable under current permissions.")
                           ->setOptional()}})
      ->required({"code"});
}

shared::ToolResult PythonExecuteTool::execute(const PythonExecuteInput &input,
                                              shared::ToolContext &ctx) {
  static const char *kCodeEnvVar = "FIRMIUS_PYTHON_EXECUTE_CODE";

  std::string pythonExecutable = "python3";
  std::string resolvedVenv;
  std::string processId;

  try {
    if (!input.venv.empty()) {
      resolvedVenv =
          ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.venv);
      ctx.agent.getPermissions()->validatePathAccess(
          resolvedVenv, firmius::shared::AccessMode::READ);
      pythonExecutable =
          (std::filesystem::path(resolvedVenv) / "bin" / "python").string();
    }

    std::string command =
        pythonExecutable +
        " -c \"import os; exec(compile(os.environ['" +
        std::string(kCodeEnvVar) +
        "'], '<python_execute>', 'exec'))\"";

    std::string effectiveCwd = ctx.agent.getContext().environment.cwd;
    effectiveCwd =
        ctx.agent.getEnvironment()->getWorkspace().resolvePath(effectiveCwd);

    ctx.agent.getPermissions()->validatePathAccess(
        effectiveCwd, firmius::shared::AccessMode::READ);
    auto intent = ctx.agent.getPermissions()->getIntentAnalyzer().analyze(
        command, effectiveCwd);
    auto approval =
        ctx.agent.getPermissions()->requestCommandApproval(command, intent, "Python");
    if (approval == PermissionResponse::Deny) {
      return shared::ToolResult::fail("Command execution denied: " + command);
    }

    std::map<std::string, std::string> env = {
        {kCodeEnvVar, input.code},
        {"FIRMIUS_PYTHON_EXECUTE_LINES",
         std::to_string(std::count(input.code.begin(), input.code.end(), '\n') +
                        (!input.code.empty() ? 1 : 0))}};
    if (!resolvedVenv.empty()) {
      env["VIRTUAL_ENV"] = resolvedVenv;
    }

    processId = ctx.agent.getEnvironment()->getProcessManager().spawnProcess(
        command, ctx.currentToolCallId, effectiveCwd, env);
    ctx.agent.getEnvironment()->getProcessManager().addBlockingProcessId(processId);

    shared::ProcessSnapshot snap;
    auto sleepDuration = std::chrono::milliseconds(1);
    const auto maxSleep = std::chrono::milliseconds(20);

    while (true) {
      if (ctx.cancelRequested()) {
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(
            processId);
        rapidjson::Document doc;
        doc.SetObject();
        snap.exitCode = -2;
        buildPyResultDoc(doc, snap, "Interrupted", processId);
        return shared::ToolResult::ok(doc, processId);
      }

      snap =
          ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
      if (!snap.running) {
        break;
      }

      if (!ctx.waitFor(sleepDuration)) {
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(
            processId);
        rapidjson::Document doc;
        doc.SetObject();
        snap.exitCode = -2;
        buildPyResultDoc(doc, snap, "Interrupted", processId);
        return shared::ToolResult::ok(doc, processId);
      }

      if (sleepDuration < maxSleep) {
        sleepDuration = std::min(maxSleep, sleepDuration * 2);
      }
    }

    ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(
        processId);

    rapidjson::Document doc;
    doc.SetObject();
    buildPyResultDoc(doc, snap, "Natural", processId);
    return shared::ToolResult::ok(doc, processId);
  } catch (const std::exception &e) {
    if (!processId.empty()) {
      ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(
          processId);
    }
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
