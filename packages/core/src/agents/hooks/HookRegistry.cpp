#include "agents/hooks/HookRegistry.hpp"

#include "agents/hooks/HookEnvelope.hpp"
#include "agents/hooks/HookState.hpp"
#include "agents/hooks/ScriptRuntime.hpp"
#include "agents/hooks/TemplateEngine.hpp"
#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "workflow/WorkflowLoader.hpp"
#include "utils/StringUtil.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <rapidjson/document.h>
#include <rapidjson/pointer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sys/wait.h>
#include <thread>

namespace firmius::core::hooks {

namespace {

int defaultShellRunner(const std::string &command, int timeoutSec,
                       std::string *stdoutOut, std::string *stderrOut) {
  if (stdoutOut) {
    stdoutOut->clear();
  }
  if (stderrOut) {
    stderrOut->clear();
  }

  std::string fullCommand = command;
  if (fullCommand.find("2>") == std::string::npos) {
    fullCommand += " 2>&1";
  }

  std::FILE *pipe = ::popen(fullCommand.c_str(), "r");
  if (!pipe) {
    if (stderrOut) {
      *stderrOut = "popen failed";
    }
    return -1;
  }

  std::atomic<bool> done{false};
  std::string captured;
  std::thread reader([&]() {
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) !=
           nullptr) {
      captured.append(buf.data());
    }
    done.store(true);
  });

  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::seconds(timeoutSec);
  while (!done.load() && clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  int exitCode = 0;
  if (!done.load()) {
    ::pclose(pipe);
    if (reader.joinable()) {
      reader.detach();
    }
    if (stderrOut) {
      *stderrOut = "hook timed out after " + std::to_string(timeoutSec) + "s";
    }
    return 124;
  }

  if (reader.joinable()) {
    reader.join();
  }
  exitCode = ::pclose(pipe);
  if (exitCode != -1) {
    if (WIFEXITED(exitCode)) {
      exitCode = WEXITSTATUS(exitCode);
    }
  }

  if (stdoutOut) {
    *stdoutOut = captured;
  }
  return exitCode;
}

std::string stringifyJsonValue(const rapidjson::Value &v) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  v.Accept(w);
  return std::string(sb.GetString(), sb.GetSize());
}

std::optional<std::string>
resolveMatchValue(const std::string &key, const EventPayload &p) {
  if (key == "tool")
    return p.toolName;
  if ((key == "success" || key == "tool_success") && p.toolSuccess.has_value())
    return *p.toolSuccess ? std::string{"true"} : std::string{"false"};
  if (key == "persona")
    return p.persona;
  if (key == "mode")
    return p.activeMode;
  if (key == "from_mode")
    return p.fromMode;
  if (key == "to_mode")
    return p.toMode;
  if (key == "pact_verdict")
    return p.pactVerdict;
  if (key == "pact_id")
    return p.pactId;
  if (key == "thread_id")
    return p.threadId;
  if (key == "agent_id")
    return p.agentId;
  if (key == "completed_workflow")
    return p.completedWorkflowId;
  if (key == "subagent_branch_id")
    return p.subagentBranchId;
  auto it = p.extra.find(key);
  if (it != p.extra.end())
    return it->second;
  return std::nullopt;
}

std::string stateKeyToPointer(std::string key) {
  if (key.rfind("state.", 0) == 0) {
    key = key.substr(6);
  }
  std::string out = "/";
  for (char c : key) {
    if (c == '.') {
      out.push_back('/');
    } else if (c == '~') {
      out += "~0";
    } else if (c == '/') {
      out += "~1";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::optional<std::string> resolveStateMatchValue(const std::string &key,
                                                  const std::string &hookId) {
  if (key.rfind("state.", 0) != 0) {
    return std::nullopt;
  }
  rapidjson::Document doc;
  const auto snapshot = HookState::instance().snapshotJson(hookId);
  if (doc.Parse(snapshot.c_str()).HasParseError() || !doc.IsObject()) {
    return std::nullopt;
  }
  rapidjson::Pointer ptr(stateKeyToPointer(key).c_str());
  const rapidjson::Value *v = ptr.Get(doc);
  if (v == nullptr) {
    return std::nullopt;
  }
  if (v->IsString()) {
    return std::string(v->GetString(), v->GetStringLength());
  }
  if (v->IsBool()) {
    return v->GetBool() ? std::string{"true"} : std::string{"false"};
  }
  if (v->IsInt64()) {
    return std::to_string(v->GetInt64());
  }
  if (v->IsUint64()) {
    return std::to_string(v->GetUint64());
  }
  if (v->IsNumber()) {
    return std::to_string(v->GetDouble());
  }
  return stringifyJsonValue(*v);
}

HookDispatcher::ShellRunner &runnerSlot() {
  static HookDispatcher::ShellRunner runner = defaultShellRunner;
  return runner;
}

std::mutex &runnerMutex() {
  static std::mutex m;
  return m;
}

std::map<std::string, std::string> makeExtras(const EventPayload &p) {
  std::map<std::string, std::string> extras = p.extra;
  extras["tool"] = p.toolName;
  extras["persona"] = p.persona;
  extras["thread_id"] = p.threadId;
  extras["agent_id"] = p.agentId;
  extras["active_mode"] = p.activeMode;
  extras["from_mode"] = p.fromMode;
  extras["to_mode"] = p.toMode;
  extras["pact_id"] = p.pactId;
  extras["pact_verdict"] = p.pactVerdict;
  extras["completed_workflow"] = p.completedWorkflowId;
  extras["subagent_branch_id"] = p.subagentBranchId;
  return extras;
}

TemplateContext makeContext(const Workflow &hook, WorkflowEventKind eventKind,
                            const EventPayload &p,
                            const std::string &subagentReturnJson = "{}") {
  const std::string effectiveSubagentReturnJson =
      !subagentReturnJson.empty() && subagentReturnJson != "{}"
          ? subagentReturnJson
          : (!p.returnPayloadJson.empty() ? p.returnPayloadJson
                                          : std::string{"{}"});
  const std::string stateJson = HookState::instance().snapshotJson(hook.id);
  HookEnvelope env = buildEnvelope(hook.id, eventKind, p, stateJson);
  return makeTemplateContext(stateJson, serializeEnvelope(env), p.toolArgsJson,
                             effectiveSubagentReturnJson, makeExtras(p));
}

bool truthy(std::string value) {
  for (char &c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return !(value.empty() || value == "0" || value == "false" || value == "null" ||
           value == "no" || value == "off");
}

void applyOutcomeEffects(const Workflow &hook, const TemplateContext &ctx,
                         const std::optional<WorkflowEmit> &emit,
                         HookOutcome &out) {
  if (!emit.has_value())
    return;

  if (!emit->outcomeTemplate.empty()) {
    out.outcomeLabel = renderTemplate(emit->outcomeTemplate, ctx);
  }

  for (const auto &w : emit->stateWrites) {
    HookOutcome::StateWrite sw;
    sw.scope = w.scope;
    sw.path = w.path;
    sw.valueJson = renderTemplate(w.valueTemplate, ctx);
    out.stateWrites.push_back(std::move(sw));
  }

  if (emit->blockDecision.has_value()) {
    const auto &bd = *emit->blockDecision;
    const bool cond = truthy(renderTemplate(bd.condition, ctx));
    const std::string branch = cond ? bd.thenBranch : bd.elseBranch;
    if (branch == "block") {
      out.decision = HookOutcome::Decision::Block;
      if (out.blockReason.empty()) {
        out.blockReason = "blocked by hook " + hook.id;
      }
      if (!bd.injectToAgent.empty()) {
        out.reminderForAgent = renderTemplate(bd.injectToAgent, ctx);
      }
    } else if (branch == "allow") {
      out.decision = HookOutcome::Decision::Allow;
    }
  }

}

// ---- action runners ---------------------------------------------------------

HookOutcome runShellAction(const Workflow &hook, WorkflowEventKind eventKind,
                           const EventPayload &p) {
  HookOutcome out;
  const TemplateContext ctx = makeContext(hook, eventKind, p);

  std::string command = renderTemplate(hook.action.command, ctx);
  std::string stdoutStr;
  std::string stderrStr;

  int rc = 0;
  {
    std::lock_guard<std::mutex> lock(runnerMutex());
    rc = runnerSlot()(command, hook.action.timeoutSec, &stdoutStr, &stderrStr);
  }

  out = parseHookOutcome(hook.id, eventKind, rc, stdoutStr, stderrStr,
                         hook.action.claudeCodeCompat);

  applyOutcomeEffects(hook, ctx, hook.emit, out);
  return out;
}

HookOutcome runPromptAction(const Workflow &hook, WorkflowEventKind eventKind,
                            const EventPayload &p) {
  HookOutcome out;
  const TemplateContext ctx = makeContext(hook, eventKind, p);

  // The prompt action is rendered and returned to the agent as a reminder.
  // Agent-side logic treats it as an injected system reminder.
  const std::string prompt =
      renderTemplate(hook.action.scriptBody.empty() ? hook.body
                                                    : hook.action.scriptBody,
                     ctx);
  out.outcomeLabel = "prompt";
  out.reminderForAgent = prompt;
  applyOutcomeEffects(hook, ctx, hook.emit, out);
  return out;
}

HookOutcome runStateAction(const Workflow &hook, WorkflowEventKind eventKind,
                           const EventPayload &p) {
  HookOutcome out;
  const TemplateContext ctx = makeContext(hook, eventKind, p);
  out.outcomeLabel = "state";
  applyOutcomeEffects(hook, ctx, hook.emit, out);
  return out;
}

HookOutcome runScriptAction(const Workflow &hook, WorkflowEventKind eventKind,
                            const EventPayload &p) {
  HookOutcome out;
  const TemplateContext ctx = makeContext(hook, eventKind, p);

  try {
    std::string scriptBody = hook.action.scriptBody;
    if (!hook.action.scriptFile.empty()) {
      std::filesystem::path scriptPath(hook.action.scriptFile);
      if (scriptPath.is_relative()) {
        scriptPath = std::filesystem::path(hook.sourceDir) / scriptPath;
      }
      std::ifstream in(scriptPath);
      if (!in.is_open()) {
        throw std::runtime_error("could not open script_file " +
                                 scriptPath.string());
      }
      std::ostringstream body;
      body << in.rdbuf();
      scriptBody = body.str();
    }
    const std::string stateJson = HookState::instance().snapshotJson(hook.id);
    HookEnvelope env = buildEnvelope(hook.id, eventKind, p, stateJson);
    auto runtime = ScriptRuntime::create();
    out = runtime->eval(hook.id, renderTemplate(scriptBody, ctx), env);
    applyOutcomeEffects(hook, ctx, hook.emit, out);
  } catch (const std::exception &e) {
    out.outcomeLabel = "script_failed";
    out.reminderForAgent = "<FIRMIUS_HOOK id=\"" + hook.id +
                           "\" exit=\"-1\">script hook action failed: " +
                           std::string(e.what()) + "</FIRMIUS_HOOK>";
  }

  return out;
}

HookOutcome runComposeAction(const Workflow &hook, WorkflowEventKind eventKind,
                            const EventPayload &p) {
  HookOutcome out;
  const TemplateContext ctx = makeContext(hook, eventKind, p);

  out.outcomeLabel = "compose";
  // Minimal implementation: thread outcomes sequentially; last step wins.
  for (const auto &step : hook.action.composeSteps) {
    Workflow sub = hook;
    sub.id = hook.id + "::step";
    sub.action = WorkflowAction{};
    sub.action.kind = workflowActionKindFromString(step.kind);
    if (sub.action.kind == WorkflowActionKind::Shell) {
      auto it = step.params.find("command");
      sub.action.command = it != step.params.end() ? it->second : step.body;
    } else if (sub.action.kind == WorkflowActionKind::Prompt ||
               sub.action.kind == WorkflowActionKind::Tool) {
      sub.body = step.body;
    } else if (sub.action.kind == WorkflowActionKind::State) {
      sub.action.stateWrites = step.stateWrites;
    }
    HookOutcome stepOut = HookDispatcher::runAction(sub, p);
    out = stepOut;
  }
  applyOutcomeEffects(hook, ctx, hook.emit, out);
  return out;
}

HookOutcome runWorkflowAction(const Workflow &hook, WorkflowEventKind eventKind,
                             const EventPayload &p) {
  HookOutcome out;
  const TemplateContext ctx = makeContext(hook, eventKind, p);

  // Workflows are executed synchronously by pushing the built prompt into the
  // focused agent.
  try {
    auto workflowId = renderTemplate(hook.action.targetWorkflow, ctx);
    out.outcomeLabel = "workflow";
    out.reminderForAgent = "<FIRMIUS_HOOK id=\"" + hook.id +
                           "\" exit=\"0\">workflow action: " + workflowId +
                           "</FIRMIUS_HOOK>";

    // We execute via engine/harness on the focused agent. This matches previous
    // behavior (HookDispatcher is used by Agent runtime, which owns the context).
    // Any errors are captured as reminders.
    (void)workflowId;

    applyOutcomeEffects(hook, ctx, hook.emit, out);
  } catch (const std::exception &e) {
    out.outcomeLabel = "workflow_failed";
    out.reminderForAgent = "<FIRMIUS_HOOK id=\"" + hook.id +
                           "\" exit=\"-1\">workflow hook action failed: " +
                           std::string(e.what()) + "</FIRMIUS_HOOK>";
  }

  return out;
}

HookOutcome runToolInterceptAction(const Workflow &hook, WorkflowEventKind eventKind,
                                  const EventPayload &p) {
  HookOutcome out;
  const TemplateContext ctx = makeContext(hook, eventKind, p);
  out.outcomeLabel = "tool_intercept";
  applyOutcomeEffects(hook, ctx, hook.emit, out);
  return out;
}

HookOutcome runAgentAction(const Workflow &hook, WorkflowEventKind eventKind,
                           const EventPayload &p) {
  HookOutcome out;
  const TemplateContext ctx = makeContext(hook, eventKind, p);

  try {
    const std::string persona =
        renderTemplate(hook.action.targetPersona.empty()
                           ? std::string{"witness"}
                           : hook.action.targetPersona,
                       ctx);
    std::string task = renderTemplate(hook.action.agentTask, ctx);
    const std::string mode = renderTemplate(hook.action.initialMode, ctx);
    if (!mode.empty()) {
      task = "Start in mode `" + mode + "`.\n\n" + task;
    }
    const std::string childId = Engine::instance().summonAgent(
        p.threadId, persona, task, true, p.agentId, persona, "Hook Validator");
    out.spawnedAgentId = childId;
    const auto timeout =
        std::chrono::seconds(hook.action.timeoutSec > 0 ? hook.action.timeoutSec
                                                        : 180);
    auto outcome = Engine::instance().waitForAgentOutcome(childId, timeout);
    rapidjson::Document ret;
    ret.SetObject();
    auto &alloc = ret.GetAllocator();
    ret.AddMember("agent_id", rapidjson::Value(childId.c_str(), alloc).Move(),
                  alloc);
    if (outcome.has_value()) {
      std::string kind = "response";
      switch (outcome->kind) {
      case AgentOutcome::Kind::Response:
        kind = "response";
        break;
      case AgentOutcome::Kind::NoSummary:
        kind = "no_summary";
        break;
      case AgentOutcome::Kind::Cancelled:
        kind = "cancelled";
        break;
      case AgentOutcome::Kind::Failed:
        kind = "failed";
        break;
      }
      ret.AddMember("kind", rapidjson::Value(kind.c_str(), alloc).Move(),
                    alloc);
      ret.AddMember("text",
                    rapidjson::Value(outcome->text.c_str(), alloc).Move(),
                    alloc);
      rapidjson::Document parsed;
      if (!outcome->text.empty() &&
          !parsed.Parse(outcome->text.c_str()).HasParseError() &&
          parsed.IsObject()) {
        ret.AddMember("json", parsed.Move(), alloc);
      }
    } else {
      ret.AddMember("kind", "timeout", alloc);
      ret.AddMember("text", "validator timed out", alloc);
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    ret.Accept(writer);
    out.spawnedAgentReturnJson = std::string(sb.GetString(), sb.GetSize());
    out.outcomeLabel = "agent";
    const TemplateContext returnCtx =
        makeContext(hook, eventKind, p, out.spawnedAgentReturnJson);
    applyOutcomeEffects(hook, returnCtx, hook.emit, out);
    return out;
  } catch (const std::exception &e) {
    out.outcomeLabel = "agent_spawn_failed";
    out.reminderForAgent = "<FIRMIUS_HOOK id=\"" + hook.id +
                           "\" exit=\"-1\">agent hook action failed: " +
                           std::string(e.what()) + "</FIRMIUS_HOOK>";
  }

  applyOutcomeEffects(hook, ctx, hook.emit, out);
  return out;
}

void settleOutcomeImpl(const Workflow &hook, HookOutcome &out) {
  if (!out.stateWrites.empty()) {
    std::vector<HookState::BatchWrite> writes;
    writes.reserve(out.stateWrites.size());
    for (const auto &w : out.stateWrites) {
      HookState::BatchWrite bw;
      bw.scope = parseScope(w.scope);
      bw.path = w.path;
      bw.valueJson = w.valueJson;
      bw.append = w.path.size() >= 2 && w.path.substr(w.path.size() - 2) == "[]";
      writes.push_back(std::move(bw));
    }
    HookState::instance().applyBatch(writes, hook.id);
  }
}

} // namespace

HookRegistry &HookRegistry::instance() {
  static HookRegistry reg;
  static std::once_flag once;
  std::call_once(once, [&]() { reg.reload(); });
  return reg;
}

void HookRegistry::reload() {
  std::lock_guard<std::mutex> lock(mu_);
  byEvent_.clear();

  const auto &loader = WorkflowLoader::instance();
  for (const auto &id : loader.getWorkflowIds()) {
    const Workflow *wf = loader.getWorkflow(id);
    if (!wf || !wf->isHook()) {
      continue;
    }
    if (wf->trigger.event == WorkflowEventKind::Unknown) {
      std::cerr << "[hooks] '" << wf->id
                << "' declares trigger.on_event but the kind is unknown — skipping. Check spelling.\n";
      continue;
    }
    byEvent_[wf->trigger.event].push_back(wf);
  }
}

std::vector<const Workflow *> HookRegistry::hooksFor(WorkflowEventKind kind) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = byEvent_.find(kind);
  if (it == byEvent_.end()) {
    return {};
  }
  return it->second;
}

std::size_t HookRegistry::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::size_t total = 0;
  for (const auto &[_, v] : byEvent_) {
    total += v.size();
  }
  return total;
}

bool HookDispatcher::matches(const Workflow &hook, const EventPayload &p) {
  // Presence predicates: key must resolve (present=true) or must be absent
  // (present=false).
  for (const auto &[k, requiredPresent] : hook.trigger.match.present) {
    const bool hasValue =
        (k.rfind("state.", 0) == 0 ? resolveStateMatchValue(k, hook.id)
                                   : resolveMatchValue(k, p))
            .has_value();
    if (requiredPresent && !hasValue) {
      return false;
    }
    if (!requiredPresent && hasValue) {
      return false;
    }
  }

  for (const auto &[k, expected] : hook.trigger.match.equals) {
    auto observed = k.rfind("state.", 0) == 0
                        ? resolveStateMatchValue(k, hook.id)
                        : resolveMatchValue(k, p);
    if (!observed.has_value()) {
      return false;
    }
    if (*observed != expected) {
      return false;
    }
  }
  return true;
}

HookOutcome HookDispatcher::runAction(const Workflow &hook,
                                      const EventPayload &payload) {
  switch (hook.action.kind) {
  case WorkflowActionKind::Shell:
    return runShellAction(hook, hook.trigger.event, payload);
  case WorkflowActionKind::Prompt:
  case WorkflowActionKind::Tool:
    return runPromptAction(hook, hook.trigger.event, payload);
  case WorkflowActionKind::State:
    return runStateAction(hook, hook.trigger.event, payload);
  case WorkflowActionKind::Script:
    return runScriptAction(hook, hook.trigger.event, payload);
  case WorkflowActionKind::Compose:
    return runComposeAction(hook, hook.trigger.event, payload);
  case WorkflowActionKind::Workflow:
    return runWorkflowAction(hook, hook.trigger.event, payload);
  case WorkflowActionKind::ToolIntercept:
    return runToolInterceptAction(hook, hook.trigger.event, payload);
  case WorkflowActionKind::Agent:
    return runAgentAction(hook, hook.trigger.event, payload);
  }
  return HookOutcome{};
}

void HookDispatcher::settleOutcome(const Workflow &hook, HookOutcome &out) {
  settleOutcomeImpl(hook, out);
}

EventResult HookDispatcher::fire(WorkflowEventKind kind,
                                 const EventPayload &payload) {
  EventResult result;
  const auto hooks = HookRegistry::instance().hooksFor(kind);
  if (hooks.empty()) {
    return result;
  }
  const bool isBlockable = workflowEventIsBlockable(kind);

  for (const Workflow *hook : hooks) {
    if (!hook) {
      continue;
    }
    if (!matches(*hook, payload)) {
      continue;
    }

    HookOutcome outcome = runAction(*hook, payload);
    settleOutcomeImpl(*hook, outcome);
    if (!result.firstOutcome.has_value()) {
      result.firstOutcome = outcome;
    }
    if (outcome.reminderForAgent.has_value()) {
      result.injectedReminders.push_back(*outcome.reminderForAgent);
    }
    result.firedHookIds.push_back(hook->id);
    if (!outcome.replacementToolArgs.empty()) {
      result.replacementToolArgs = outcome.replacementToolArgs;
    }

    if (isBlockable && hook->trigger.block &&
        outcome.decision == HookOutcome::Decision::Block) {
      result.blocked = true;
      result.blockReason = outcome.blockReason.empty()
                               ? ("blocked by hook " + hook->id)
                               : outcome.blockReason;
      break;
    }
  }
  return result;
}

} // namespace firmius::core::hooks
