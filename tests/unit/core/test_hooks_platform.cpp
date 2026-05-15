#include "agents/hooks/HookEnvelope.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "agents/hooks/HookState.hpp"
#include "agents/hooks/ScriptRuntime.hpp"
#include "AgentRegistry.hpp"
#include "workflow/Workflow.hpp"
#include "workflow/WorkflowLoader.hpp"
#include "../mocks/MockAgent.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace firmius::core::hooks {
namespace {

std::string makeTempDir(const std::string &prefix) {
  std::filesystem::path base = std::filesystem::temp_directory_path() /
                               (prefix + std::to_string(std::rand()));
  std::filesystem::create_directories(base);
  return base.string();
}

struct HookEnvGuard {
  std::string oldHome;
  bool hadHome = false;
  std::string oldFirmiusHome;
  bool hadFirmiusHome = false;
  std::string oldHooksDir;
  bool hadHooksDir = false;
  std::string oldWorkflowsDir;
  bool hadWorkflowsDir = false;

  explicit HookEnvGuard(const std::string &home) {
    if (const char *v = std::getenv("HOME")) {
      oldHome = v;
      hadHome = true;
    }
    if (const char *v = std::getenv("FIRMIUS_HOME")) {
      oldFirmiusHome = v;
      hadFirmiusHome = true;
    }
    if (const char *v = std::getenv("FIRMIUS_HOOKS_DIR")) {
      oldHooksDir = v;
      hadHooksDir = true;
    }
    if (const char *v = std::getenv("FIRMIUS_WORKFLOWS_DIR")) {
      oldWorkflowsDir = v;
      hadWorkflowsDir = true;
    }
    ::setenv("HOME", home.c_str(), 1);
    ::setenv("FIRMIUS_HOME", home.c_str(), 1);
  }

  ~HookEnvGuard() {
    if (hadHome)
      ::setenv("HOME", oldHome.c_str(), 1);
    else
      ::unsetenv("HOME");
    if (hadFirmiusHome)
      ::setenv("FIRMIUS_HOME", oldFirmiusHome.c_str(), 1);
    else
      ::unsetenv("FIRMIUS_HOME");
    if (hadHooksDir)
      ::setenv("FIRMIUS_HOOKS_DIR", oldHooksDir.c_str(), 1);
    else
      ::unsetenv("FIRMIUS_HOOKS_DIR");
    if (hadWorkflowsDir)
      ::setenv("FIRMIUS_WORKFLOWS_DIR", oldWorkflowsDir.c_str(), 1);
    else
      ::unsetenv("FIRMIUS_WORKFLOWS_DIR");
  }
};

EventPayload makePayload() {
  EventPayload p;
  p.threadId = "thread-1";
  p.agentId = "agent-1";
  p.persona = "forge";
  p.activeMode = "forge:apply";
  p.toolName = "Files.Edit";
  p.toolArgsJson = R"({"path":"src/demo.cpp","value":7})";
  p.toolResultJson = R"({"ok":true})";
  p.toolSuccess = true;
  p.extra["custom"] = "anchor";
  return p;
}

TEST(HookStateRegression, PersistsThreadScopedWritesAcrossRebind) {
  const std::string tempHome = makeTempDir("firmius-hookstate-");
  HookEnvGuard guard(tempHome);

  auto &state = HookState::instance();
  state.bindThread("thread-a");
  ASSERT_TRUE(state.writeJson(HookState::Scope::Thread, "promise.iteration", "3"));
  ASSERT_TRUE(state.appendJson(HookState::Scope::Thread, "promise.history[]",
                               R"({"verdict":"reject"})"));
  state.unbindThread("thread-a");
  state.bindThread("thread-a");

  const auto iteration = state.readJson(HookState::Scope::Thread,
                                        "promise.iteration");
  ASSERT_TRUE(iteration.has_value());
  EXPECT_EQ(*iteration, "3");

  const auto history = state.readJson(HookState::Scope::Thread,
                                      "promise.history");
  ASSERT_TRUE(history.has_value());
  EXPECT_NE(history->find("reject"), std::string::npos);
}

TEST(ScriptRuntimeE2E, LuauReadsEnvelopeAndProducesStructuredOutcome) {
  if (!ScriptRuntime::enabled()) {
    GTEST_SKIP() << "Luau hooks disabled in this build";
  }

  const std::string tempHome = makeTempDir("firmius-luau-");
  HookEnvGuard guard(tempHome);
  HookState::instance().bindThread("thread-luau");
  ASSERT_TRUE(HookState::instance().writeJson(HookState::Scope::Thread,
                                              "promise.iteration", "2",
                                              "hook.luau"));

  const EventPayload payload = makePayload();
  const std::string snapshot = HookState::instance().snapshotJson("hook.luau");
  HookEnvelope env = buildEnvelope("hook.luau", WorkflowEventKind::PreToolUse,
                                   payload, snapshot);

  auto rt = ScriptRuntime::create();
  HookOutcome out = rt->eval(
      "hook.luau",
      R"(
        local iter = state.read("thread", "promise.iteration")
        state.write("thread", "promise.last_api_check", "ok")
        local log = thread.log_summary()
        return outcome.block{
          reason = "iteration=" .. tostring(iter) .. " agent=" .. tostring(log.agent_id),
          reminder = "luau reminder",
          outcome = "reject",
          args = { path = event.payload.tool_args.path },
        }
      )",
      env);

  EXPECT_EQ(out.decision, HookOutcome::Decision::Block);
  ASSERT_TRUE(out.reminderForAgent.has_value());
  EXPECT_EQ(*out.reminderForAgent, "luau reminder");
  EXPECT_EQ(out.blockReason, "iteration=2 agent=agent-1");
  EXPECT_EQ(out.outcomeLabel, "reject");
  EXPECT_NE(out.replacementToolArgs.find("src/demo.cpp"), std::string::npos);
  auto wrote = HookState::instance().readJson(HookState::Scope::Thread,
                                              "promise.last_api_check",
                                              "hook.luau");
  ASSERT_TRUE(wrote.has_value());
  EXPECT_EQ(*wrote, R"("ok")");
}

TEST(ScriptRuntimeE2E, LuauCanInspectMessagesAndToolEvidence) {
  if (!ScriptRuntime::enabled()) {
    GTEST_SKIP() << "Luau hooks disabled in this build";
  }

  firmius::shared::AgentContext ctx;
  ctx.identity.id = "agent-history";
  ctx.history = std::make_shared<firmius::shared::AgentHistory>();
  ctx.history->threadId = "thread-history";

  firmius::shared::AgentTurn turn;
  turn.turnId = "turn-1";
  firmius::shared::Message user;
  user.id = "msg-user";
  user.role = firmius::shared::Role::User;
  user.content.push_back(firmius::shared::TextContent{"please inspect"});
  turn.messages.push_back(user);

  firmius::shared::Message assistant;
  assistant.id = "msg-assistant";
  assistant.role = firmius::shared::Role::Assistant;
  assistant.content.push_back(
      firmius::shared::ToolCallContent{"call-1", "Process",
                                       R"({"command":"cmake --build build"})"});
  turn.messages.push_back(assistant);

  firmius::shared::Message result;
  result.id = "msg-result";
  result.role = firmius::shared::Role::ToolResult;
  result.content.push_back(firmius::shared::ToolResultContent{
      "call-1", "build ok", true, "proc-1", ""});
  turn.messages.push_back(result);
  ctx.history->turns.push_back(turn);

  auto agent = std::make_shared<firmius::test::MockAgent>(ctx);
  AgentRegistry::instance().registerAgent("agent-history", agent);

  EventPayload payload;
  payload.threadId = "thread-history";
  payload.agentId = "agent-history";
  HookEnvelope env = buildEnvelope("hook.history", WorkflowEventKind::AgentStop,
                                   payload, "{}");

  auto rt = ScriptRuntime::create();
  HookOutcome out = rt->eval(
      "hook.history",
      R"(
        local messages = thread.messages({ role = "user" })
        local calls = thread.tool_calls({ tool = "Process" })
        local results = thread.tool_results({ success = true })
        return outcome.allow{
          text = tostring(#messages) .. ":" ..
                 tostring(calls[1].name) .. ":" ..
                 tostring(calls[1].result.result) .. ":" ..
                 tostring(#results)
        }
      )",
      env);

  AgentRegistry::instance().unregisterAgent("agent-history");

  ASSERT_TRUE(out.reminderForAgent.has_value());
  EXPECT_EQ(*out.reminderForAgent, "1:Process:build ok:1");
}

TEST(HookPackLoaderRegression, LoadsYamlHooksAndMatchesThreadState) {
  const std::string tempHome = makeTempDir("firmius-hookpack-");
  HookEnvGuard guard(tempHome);
  const auto hooksDir = std::filesystem::path(tempHome) / "hooks";
  const auto workflowsDir = std::filesystem::path(tempHome) / "workflows";
  std::filesystem::create_directories(hooksDir / "promise" / "flows");
  std::filesystem::create_directories(workflowsDir);
  ::setenv("FIRMIUS_HOOKS_DIR", hooksDir.c_str(), 1);
  ::setenv("FIRMIUS_WORKFLOWS_DIR", workflowsDir.c_str(), 1);

  std::ofstream flow(hooksDir / "promise" / "flows" / "stop.yaml");
  flow << R"(id: promise.flow.stop
trigger:
  on_event: agent_stop
  match:
    state.thread.promise.state: { equals: open }
  block: true
action:
  kind: script
  body: "return outcome.block{ reason = 'open promise', reminder = 'seal it' }"
)";
  flow.close();

  WorkflowLoader::instance().init();
  HookRegistry::instance().reload();

  auto &state = HookState::instance();
  state.bindThread("thread-pack");
  ASSERT_TRUE(state.writeJson(HookState::Scope::Thread, "promise.state",
                              R"("open")", "promise.flow.stop"));

  EventPayload payload;
  payload.threadId = "thread-pack";
  payload.agentId = "agent-pack";
  auto result = HookDispatcher::fire(WorkflowEventKind::AgentStop, payload);
  EXPECT_TRUE(result.blocked);
  ASSERT_EQ(result.injectedReminders.size(), 1u);
  EXPECT_EQ(result.injectedReminders.front(), "seal it");
}

TEST(HookPackLoaderRegression, LoadsInstalledPromptHookPacksByDefault) {
  const std::string tempHome = makeTempDir("firmius-installed-hookpack-");
  HookEnvGuard guard(tempHome);
  ::unsetenv("FIRMIUS_HOOKS_DIR");
  ::setenv("FIRMIUS_WORKFLOWS_DIR",
           (std::filesystem::path(tempHome) / "workflows").c_str(), 1);

  const auto hookDir = std::filesystem::path(tempHome) / ".firmius" /
                       "prompts" / "hooks" / "promise" / "commands";
  std::filesystem::create_directories(hookDir);
  std::filesystem::create_directories(std::filesystem::path(tempHome) /
                                      ".firmius" / "prompts" / "hooks" /
                                      "promise" / "flows");
  std::filesystem::create_directories(std::filesystem::path(tempHome) /
                                      "workflows");

  std::ofstream manifest(std::filesystem::path(tempHome) / ".firmius" /
                         "prompts" / "hooks" / "promise" / "pack.yaml");
  manifest << R"(id: promise
files:
  - commands/promise.yaml
)";
  manifest.close();

  std::ofstream command(hookDir / "promise.yaml");
  command << R"(id: promise.command.promise
name: Promise
slash_command: /promise
raw_remainder: true
action:
  kind: script
  body: "return outcome.allow{ text = event.payload.extra.raw_args }"
)";
  command.close();

  std::ofstream stale(std::filesystem::path(tempHome) / ".firmius" /
                      "prompts" / "hooks" / "promise" / "flows" /
                      "stale.yaml");
  stale << "id: stale\nthis: [is: invalid\n";
  stale.close();

  WorkflowLoader::instance().init();
  const Workflow *workflow =
      WorkflowLoader::instance().getWorkflow("promise.command.promise");
  ASSERT_NE(workflow, nullptr);
  EXPECT_EQ(workflow->slashCommand, "/promise");
  EXPECT_TRUE(workflow->rawRemainder);
}

TEST(HookEnvelopeRegression, ParsesStructuredShellOutcome) {
  HookOutcome out = parseHookOutcome(
      "hook.shell", WorkflowEventKind::PreToolUse, 2,
      R"({"decision":"block","reason":"deny","outcome":"denied","state_writes":[{"scope":"thread","path":"promise.iteration","value":4}]})",
      "", true);

  EXPECT_EQ(out.decision, HookOutcome::Decision::Block);
  EXPECT_EQ(out.blockReason, "deny");
  EXPECT_EQ(out.outcomeLabel, "denied");
  ASSERT_EQ(out.stateWrites.size(), 1u);
  EXPECT_EQ(out.stateWrites[0].scope, "thread");
  EXPECT_EQ(out.stateWrites[0].path, "promise.iteration");
  EXPECT_EQ(out.stateWrites[0].valueJson, "4");
}

TEST(HookRuntimeRegression, ShellHooksReceiveEnvelopeOnStdinWhenRequested) {
  const std::string tempHome = makeTempDir("firmius-hook-shell-");
  HookEnvGuard guard(tempHome);
  const auto workflowsDir = std::filesystem::path(tempHome) / "workflows";
  std::filesystem::create_directories(workflowsDir);
  ::setenv("FIRMIUS_WORKFLOWS_DIR", workflowsDir.c_str(), 1);

  std::ofstream hookFile(workflowsDir / "shell_hook.md");
  hookFile << R"(---
name: Shell Hook
trigger:
  on_event: pre_tool_use
action:
  kind: shell
  command: cat
  pass_envelope: true
---
)";
  hookFile.close();

  WorkflowLoader::instance().init();
  HookRegistry::instance().reload();

  std::string capturedCommand;
  std::string capturedStdin;
  HookDispatcher::setShellRunner(
      [&](const std::string &command, const std::string &stdinPayload,
          int, std::string *stdoutOut, std::string *stderrOut) {
        capturedCommand = command;
        capturedStdin = stdinPayload;
        if (stdoutOut) *stdoutOut = "";
        if (stderrOut) *stderrOut = "";
        return 0;
      });

  EventPayload payload = makePayload();
  auto result = HookDispatcher::fire(WorkflowEventKind::PreToolUse, payload);
  HookDispatcher::setShellRunner({});

  ASSERT_FALSE(result.blocked);
  EXPECT_EQ(capturedCommand, "cat");
  EXPECT_NE(capturedStdin.find("\"hook_id\":\"shell_hook\""), std::string::npos);
  EXPECT_NE(capturedStdin.find("\"tool\":\"Files.Edit\""), std::string::npos);
}

TEST(HookRuntimeRegression, PackStateSurfaceRejectsOutOfBoundsWrites) {
  const std::string tempHome = makeTempDir("firmius-hook-surface-");
  HookEnvGuard guard(tempHome);
  const auto hooksDir = std::filesystem::path(tempHome) / "hooks";
  const auto workflowsDir = std::filesystem::path(tempHome) / "workflows";
  std::filesystem::create_directories(hooksDir / "pack" / "flows");
  std::filesystem::create_directories(workflowsDir);
  ::setenv("FIRMIUS_HOOKS_DIR", hooksDir.c_str(), 1);
  ::setenv("FIRMIUS_WORKFLOWS_DIR", workflowsDir.c_str(), 1);

  std::ofstream manifest(hooksDir / "pack" / "pack.yaml");
  manifest << R"(id: pack
state_surface:
  scopes: [thread]
  paths:
    - allowed.path
files:
  - flows/hook.yaml
)";
  manifest.close();

  std::ofstream flow(hooksDir / "pack" / "flows" / "hook.yaml");
  flow << R"(id: pack.flow
trigger:
  on_event: pre_tool_use
action:
  kind: state
  writes:
    - { scope: thread, path: allowed.path, value: ok }
)";
  flow.close();

  WorkflowLoader::instance().init();
  HookRegistry::instance().reload();

  HookState::instance().bindThread("thread-surface");
  EXPECT_TRUE(HookState::instance().writeJson(HookState::Scope::Thread,
                                              "allowed.path", R"("ok")",
                                              "pack.flow"));
  EXPECT_FALSE(HookState::instance().writeJson(HookState::Scope::Thread,
                                               "forbidden.path", R"("bad")",
                                               "pack.flow"));
}
} // namespace
} // namespace firmius::core::hooks
