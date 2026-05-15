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

int defaultShellRunner(const std::string &command,
                       const std::string &stdinPayload, int timeoutSec,
                       std::string *stdoutOut, std::string *stderrOut) {
  if (stdoutOut) {
    stdoutOut->clear();
  }
  if (stderrOut) {
    stderrOut->clear();
  }

  std::string fullCommand = command;
  std::filesystem::path stdinPath;
  if (!stdinPayload.empty()) {
    stdinPath = std::filesystem::temp_directory_path() /
                ("firmius-hook-stdin-" +
                 firmius::shared::StringUtil::generateUuid() + ".json");
    std::ofstream stdinFile(stdinPath, std::ios::binary | std::ios::trunc);
    if (!stdinFile.is_open()) {
      if (stderrOut) {
        *stderrOut = "failed to create hook stdin envelope file";
      }
      return -1;
    }
    stdinFile << stdinPayload;
    stdinFile.close();
    fullCommand += " < '" + stdinPath.string() + "'";
  }
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
    if (!stdinPath.empty()) {
      std::error_code ec;
      std::filesystem::remove(stdinPath, ec);
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
  if (!stdinPath.empty()) {
    std::error_code ec;
    std::filesystem::remove(stdinPath, ec);
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

TemplateContext makeContextWithExtras(
    const Workflow &hook, WorkflowEventKind eventKind, const EventPayload &p,
    const std::map<std::string, std::string> &extras,
    const std::string &subagentReturnJson = "{}") {
  auto merged = makeExtras(p);
  for (const auto &[key, value] : extras) {
    merged[key] = value;
  }
  const std::string effectiveSubagentReturnJson =
      !subagentReturnJson.empty() && subagentReturnJson != "{}"
          ? subagentReturnJson
          : (!p.returnPayloadJson.empty() ? p.returnPayloadJson
                                          : std::string{"{}"});
  const std::string stateJson = HookState::instance().snapshotJson(hook.id);
  HookEnvelope env = buildEnvelope(hook.id, eventKind, p, stateJson);
  return makeTemplateContext(stateJson, serializeEnvelope(env), p.toolArgsJson,
                             effectiveSubagentReturnJson, std::move(merged));
}

std::string decisionName(HookOutcome::Decision decision) {
  switch (decision) {
  case HookOutcome::Decision::Allow:
    return "allow";
  case HookOutcome::Decision::Block:
    return "block";
  case HookOutcome::Decision::Replace:
    return "replace";
  }
  return "allow";
}

void applyToolReturnPayload(const Workflow &hook, const TemplateContext &ctx,
                            HookOutcome &out) {
  if (hook.resultReturn.empty()) {
    return;
  }
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  for (const auto &[key, valueTemplate] : hook.resultReturn) {
    const std::string rendered = renderTemplate(valueTemplate, ctx);
    rapidjson::Document valueDoc;
    rapidjson::Value value;
    if (!rendered.empty() && !valueDoc.Parse(rendered.c_str()).HasParseError()) {
      value.CopyFrom(valueDoc, alloc);
    } else {
      value.SetString(rendered.c_str(),
                      static_cast<rapidjson::SizeType>(rendered.size()),
                      alloc);
    }
    rapidjson::Value keyValue(key.c_str(),
                              static_cast<rapidjson::SizeType>(key.size()),
                              alloc);
    doc.AddMember(keyValue.Move(), value.Move(), alloc);
  }
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);
  out.toolReturnPayloadJson = std::string(sb.GetString(), sb.GetSize());
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
  applyToolReturnPayload(hook, ctx, out);
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
  std::string stdinPayload;
  if (hook.action.passEnvelope) {
    stdinPayload = serializeEnvelope(
        buildEnvelope(hook.id, eventKind, p,
                      HookState::instance().snapshotJson(hook.id)));
  }
  std::string stdoutStr;
  std::string stderrStr;

  int rc = 0;
  {
    std::lock_guard<std::mutex> lock(runnerMutex());
    rc = runnerSlot()(command, stdinPayload, hook.action.timeoutSec, &stdoutStr,
                      &stderrStr);
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
  out.outcomeLabel = "compose";
  HookOutcome accumulated;
  std::map<std::string, std::string> composeExtras;

  for (const auto &step : hook.action.composeSteps) {
    Workflow sub = hook;
    sub.id = hook.id + "::step:" + step.kind;
    sub.action = WorkflowAction{};
    sub.action.kind = workflowActionKindFromString(step.kind);

    EventPayload stepPayload = p;
    stepPayload.returnPayloadJson = accumulated.spawnedAgentReturnJson;
    stepPayload.extra["compose.outcome"] = accumulated.outcomeLabel;
    stepPayload.extra["compose.decision"] = decisionName(accumulated.decision);
    stepPayload.extra["compose.block_reason"] = accumulated.blockReason;
    if (accumulated.reminderForAgent.has_value()) {
      stepPayload.extra["compose.reminder"] = *accumulated.reminderForAgent;
    }
    if (!accumulated.spawnedAgentId.empty()) {
      stepPayload.extra["compose.spawned_agent_id"] = accumulated.spawnedAgentId;
    }

    const TemplateContext stepCtx = makeContextWithExtras(
        hook, eventKind, stepPayload, composeExtras,
        accumulated.spawnedAgentReturnJson);

    if (sub.action.kind == WorkflowActionKind::Shell) {
      auto it = step.params.find("command");
      sub.action.command =
          renderTemplate(it != step.params.end() ? it->second : step.body, stepCtx);
      auto pe = step.params.find("pass_envelope");
      if (pe != step.params.end()) {
        sub.action.passEnvelope = truthy(pe->second);
      }
    } else if (sub.action.kind == WorkflowActionKind::Prompt ||
               sub.action.kind == WorkflowActionKind::Tool) {
      sub.body = renderTemplate(step.body, stepCtx);
    } else if (sub.action.kind == WorkflowActionKind::State) {
      sub.action.stateWrites = step.stateWrites;
      for (auto &write : sub.action.stateWrites) {
        write.valueTemplate = renderTemplate(write.valueTemplate, stepCtx);
      }
    } else if (sub.action.kind == WorkflowActionKind::Agent) {
      if (auto persona = step.params.find("target_persona");
          persona != step.params.end()) {
        sub.action.targetPersona = renderTemplate(persona->second, stepCtx);
      }
      if (auto task = step.params.find("agent_task"); task != step.params.end()) {
        sub.action.agentTask = renderTemplate(task->second, stepCtx);
      } else {
        sub.action.agentTask = renderTemplate(step.body, stepCtx);
      }
      if (auto mode = step.params.find("initial_mode"); mode != step.params.end()) {
        sub.action.initialMode = renderTemplate(mode->second, stepCtx);
      }
      if (auto timeout = step.params.find("timeout_s");
          timeout != step.params.end()) {
        try {
          sub.action.timeoutSec = std::stoi(timeout->second);
        } catch (...) {
        }
      }
    } else if (sub.action.kind == WorkflowActionKind::Workflow) {
      if (auto target = step.params.find("target_workflow");
          target != step.params.end()) {
        sub.action.targetWorkflow = renderTemplate(target->second, stepCtx);
      }
      if (auto args = step.params.find("target_args"); args != step.params.end()) {
        sub.action.targetArgs = {renderTemplate(args->second, stepCtx)};
      }
    } else if (sub.action.kind == WorkflowActionKind::ToolIntercept) {
      if (auto args = step.params.find("replacement_args");
          args != step.params.end()) {
        sub.action.scriptBody = renderTemplate(args->second, stepCtx);
      }
    }

    HookOutcome stepOut = HookDispatcher::runAction(sub, stepPayload);
    HookDispatcher::settleOutcome(sub, stepOut);
    accumulated = stepOut;

    composeExtras["compose.outcome"] = accumulated.outcomeLabel;
    composeExtras["compose.decision"] = decisionName(accumulated.decision);
    composeExtras["compose.block_reason"] = accumulated.blockReason;
    if (accumulated.reminderForAgent.has_value()) {
      composeExtras["compose.reminder"] = *accumulated.reminderForAgent;
    }
    if (!accumulated.spawnedAgentId.empty()) {
      composeExtras["compose.spawned_agent_id"] = accumulated.spawnedAgentId;
    }

    if (accumulated.decision == HookOutcome::Decision::Block) {
      break;
    }
  }

  out = accumulated;
  const TemplateContext ctx =
      makeContextWithExtras(hook, eventKind, p, composeExtras,
                            accumulated.spawnedAgentReturnJson);
  applyOutcomeEffects(hook, ctx, hook.emit, out);
  return out;
}

HookOutcome runWorkflowAction(const Workflow &hook, WorkflowEventKind eventKind,
                             const EventPayload &p) {
  HookOutcome out;
  const TemplateContext ctx = makeContext(hook, eventKind, p);
  try {
    const std::string workflowId =
        renderTemplate(hook.action.targetWorkflow, ctx);
    const Workflow *target = WorkflowLoader::instance().getWorkflow(workflowId);
    if (target == nullptr) {
      throw std::runtime_error("unknown workflow " + workflowId);
    }

    std::vector<std::string> targetArgs;
    targetArgs.reserve(hook.action.targetArgs.size());
    for (const auto &arg : hook.action.targetArgs) {
      targetArgs.push_back(renderTemplate(arg, ctx));
    }

    std::string builtPrompt;
    try {
      builtPrompt = target->build(targetArgs);
    } catch (const std::exception &e) {
      throw std::runtime_error("workflow build failed for " + workflowId +
                               ": " + e.what());
    }

    EventPayload targetPayload = p;
    targetPayload.completedWorkflowId = target->id;
    targetPayload.userMessage = builtPrompt;
    targetPayload.extra["workflow_id"] = target->id;
    targetPayload.extra["slash_command"] =
        target->slashCommand.value_or("/" + target->id);
    if (!targetArgs.empty()) {
      targetPayload.extra["raw_args"] = targetArgs.front();
    }
    for (std::size_t i = 0; i < targetArgs.size(); ++i) {
      targetPayload.extra["arg_" + std::to_string(i + 1)] = targetArgs[i];
    }

    if (target->action.kind == WorkflowActionKind::Prompt &&
        target->action.scriptBody.empty() && target->action.scriptFile.empty() &&
        target->action.command.empty() && target->action.stateWrites.empty() &&
        target->action.composeSteps.empty() && !target->emit.has_value()) {
      out.outcomeLabel = "workflow";
      out.reminderForAgent = builtPrompt;
      applyOutcomeEffects(hook, ctx, hook.emit, out);
      return out;
    }

    out = HookDispatcher::runAction(*target, targetPayload);
    HookDispatcher::settleOutcome(*target, out);
    EventResult completed = HookDispatcher::fire(WorkflowEventKind::WorkflowComplete,
                                                 targetPayload);
    if (completed.firstOutcome.has_value() &&
        out.outcomeLabel.empty()) {
      out.outcomeLabel = completed.firstOutcome->outcomeLabel;
    }
    for (const auto &reminder : completed.injectedReminders) {
      if (reminder.empty()) {
        continue;
      }
      if (out.reminderForAgent.has_value() && !out.reminderForAgent->empty()) {
        *out.reminderForAgent += "\n" + reminder;
      } else {
        out.reminderForAgent = reminder;
      }
    }
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
  std::string replacementArgs = renderTemplate(
      hook.action.scriptBody.empty() ? hook.body : hook.action.scriptBody, ctx);
  if (replacementArgs.empty()) {
    replacementArgs = renderTemplate(hook.action.command, ctx);
  }
  if (!replacementArgs.empty()) {
    out.decision = HookOutcome::Decision::Replace;
    out.replacementToolArgs = replacementArgs;
  }
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
        p.threadId, persona, task, true, p.agentId, persona, persona);
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
    EventPayload subagentPayload = p;
    subagentPayload.subagentBranchId = childId;
    subagentPayload.returnPayloadJson = out.spawnedAgentReturnJson;
    auto subagentHooks =
        HookDispatcher::fire(WorkflowEventKind::SubagentReturn, subagentPayload);
    if (!subagentHooks.injectedReminders.empty()) {
      std::string combined;
      for (std::size_t i = 0; i < subagentHooks.injectedReminders.size(); ++i) {
        if (i > 0) {
          combined += "\n";
        }
        combined += subagentHooks.injectedReminders[i];
      }
      out.reminderForAgent = combined;
    }
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
      if (!HookRegistry::instance().isStateAccessAllowed(hook.id, w.scope,
                                                         w.path)) {
        if (out.blockReason.empty()) {
          out.blockReason =
              "hook attempted to write outside declared state surface: " +
              w.scope + ":" + w.path;
        }
        out.decision = HookOutcome::Decision::Block;
        continue;
      }
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
  byId_.clear();

  const auto &loader = WorkflowLoader::instance();
  for (const auto &id : loader.getWorkflowIds()) {
    const Workflow *wf = loader.getWorkflow(id);
    if (!wf) {
      continue;
    }
    byId_[wf->id] = wf;
    if (!wf->isHook()) {
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

bool HookRegistry::isStateAccessAllowed(const std::string &hookId,
                                        const std::string &scope,
                                        const std::string &path) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = byId_.find(hookId);
  if (it == byId_.end() || it->second == nullptr) {
    return true;
  }
  const Workflow &workflow = *it->second;

  auto matchesPath = [&](const std::vector<std::string> &allowedPaths) {
    if (allowedPaths.empty()) {
      return true;
    }
    for (const auto &allowed : allowedPaths) {
      if (allowed == path) {
        return true;
      }
      if (path.rfind(allowed, 0) == 0 &&
          (path.size() == allowed.size() || path[allowed.size()] == '.' ||
           path[allowed.size()] == '[')) {
        return true;
      }
      if (allowed.size() >= 2 &&
          allowed.substr(allowed.size() - 2) == "[]" &&
          path == allowed.substr(0, allowed.size() - 2)) {
        return true;
      }
    }
    return false;
  };

  if (workflow.packStateSurface.has_value()) {
    const auto &surface = *workflow.packStateSurface;
    if (!surface.scopes.empty() &&
        std::find(surface.scopes.begin(), surface.scopes.end(), scope) ==
            surface.scopes.end()) {
      return false;
    }
    if (!matchesPath(surface.paths)) {
      return false;
    }
  }

  if (workflow.hookState.has_value()) {
    const auto &hookState = *workflow.hookState;
    if (!hookState.scope.empty() && hookState.scope != scope) {
      return false;
    }
    if (!hookState.writes.empty() && !matchesPath(hookState.writes)) {
      return false;
    }
  }

  return true;
}

std::vector<HookActivityRecord>
HookRegistry::recentActivity(const std::string &threadId,
                             std::size_t maxCount) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = activityByThread_.find(threadId);
  if (it == activityByThread_.end()) {
    return {};
  }
  const auto &entries = it->second;
  if (entries.size() <= maxCount) {
    return entries;
  }
  return std::vector<HookActivityRecord>(entries.end() - maxCount,
                                         entries.end());
}

void HookRegistry::recordActivity(HookActivityRecord record) {
  if (record.threadId.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto &entries = activityByThread_[record.threadId];
  entries.push_back(std::move(record));
  constexpr std::size_t kMaxRecordsPerThread = 48;
  if (entries.size() > kMaxRecordsPerThread) {
    entries.erase(entries.begin(),
                  entries.begin() + (entries.size() - kMaxRecordsPerThread));
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

void HookDispatcher::setShellRunner(ShellRunner runner) {
  std::lock_guard<std::mutex> lock(runnerMutex());
  runnerSlot() = runner ? std::move(runner) : defaultShellRunner;
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

    HookActivityRecord record;
    record.hookId = hook->id;
    record.threadId = payload.threadId;
    record.agentId = payload.agentId;
    record.eventName = workflowEventKindToString(kind);
    record.decision = decisionName(outcome.decision);
    record.outcomeLabel = outcome.outcomeLabel;
    record.blockReason = outcome.blockReason;
    record.stateWriteCount = static_cast<int>(outcome.stateWrites.size());
    record.timestampMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::string line = "hook " + hook->id + ": " + record.decision;
    if (!record.outcomeLabel.empty()) {
      line += " (" + record.outcomeLabel + ")";
    }
    if (!record.blockReason.empty()) {
      line += " " + record.blockReason;
    }
    record.statusLine = std::move(line);
    HookRegistry::instance().recordActivity(std::move(record));

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
