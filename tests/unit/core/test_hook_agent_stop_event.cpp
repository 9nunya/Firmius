#include "ConfigLoader.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "Panic.hpp"
#include "Events.hpp"
#include "providers/ProviderRegistry.hpp"
#include "workflow/WorkflowLoader.hpp"
#include "persistence/ThreadManager.hpp"
#include "agents/hooks/HookState.hpp"
#include "agents/hooks/ScriptRuntime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <rapidjson/document.h>
#include <string>
#include <thread>
#include <vector>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;

namespace {

template <typename Fn>
bool waitForCondition(Fn &&fn,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return fn();
}

class AgentStopHookTest : public ::testing::Test {
protected:
  void SetUp() override {
    Panic::init();
    originalHome_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    originalPromptsDir_ = std::getenv("FIRMIUS_PROMPTS_DIR")
                              ? std::getenv("FIRMIUS_PROMPTS_DIR")
                              : "";
    originalWorkflowsDir_ = std::getenv("FIRMIUS_WORKFLOWS_DIR")
                                ? std::getenv("FIRMIUS_WORKFLOWS_DIR")
                                : "";
    originalHooksDir_ = std::getenv("FIRMIUS_HOOKS_DIR")
                            ? std::getenv("FIRMIUS_HOOKS_DIR")
                            : "";
    originalConfig_ = ConfigLoader::instance().getConfig();

    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_agent_stop_hook_" +
                 std::to_string(static_cast<long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())));
    testPromptsDir_ = testHome_ / "prompts";
    testWorkflowsDir_ = testHome_ / "workflows";
    testHooksDir_ = testHome_ / "hooks";

    std::filesystem::create_directories(testHome_ / ".firmius" / "threads");
    std::filesystem::create_directories(testPromptsDir_);
    std::filesystem::create_directories(testWorkflowsDir_);
    std::filesystem::create_directories(testHooksDir_);

    setenv("HOME", testHome_.c_str(), 1);
    setenv("FIRMIUS_HOME", testHome_.c_str(), 1);
    setenv("FIRMIUS_PROMPTS_DIR", testPromptsDir_.c_str(), 1);
    setenv("FIRMIUS_WORKFLOWS_DIR", testWorkflowsDir_.c_str(), 1);
    setenv("FIRMIUS_HOOKS_DIR", testHooksDir_.c_str(), 1);

    // Minimal persona so Engine can summon an agent without repo prompts.
    std::ofstream coderFile(testPromptsDir_ / "coder.md");
    coderFile << "---\nname: coder\ntitle: Coder\n---\nCoder identity";
    coderFile.close();
    std::ofstream shrikeFile(testPromptsDir_ / "shrike.md");
    shrikeFile << "---\nname: shrike\ntitle: Shrike\n---\nValidator identity";
    shrikeFile.close();

    // Hook on agent_stop that emits a reminder.
    std::ofstream hookFile(testWorkflowsDir_ / "agent_stop_hook.md");
    hookFile << R"(---
name: Agent Stop Hook
trigger:
  on_event: agent_stop
action:
  kind: prompt
  body: AGENT_STOP_FIRED thread={{thread_id}} agent={{agent_id}} stop_reason={{stop_reason}}
---
)";
    hookFile.close();

    // Provider that returns a single stop turn.
    provider_ = std::make_shared<OneStopProvider>("one-stop");
    ProviderRegistry::instance().registerProvider(provider_);

    auto cfg = ConfigLoader::instance().getConfig();
    cfg.defaultProviderId = provider_->getId();
    cfg.defaultModelId = provider_->listModels().front().id;
    cfg.mcpServers.clear();
    ConfigLoader::instance().updateConfig(cfg);

    WorkflowLoader::instance().init();
    hooks::HookRegistry::instance().reload();
  }

  void TearDown() override {
    Engine::instance().shutdown();
    for (const auto &agentId : AgentRegistry::instance().listAll()) {
      Engine::instance().terminateAgent(agentId);
    }
    for (const auto &agentId : AgentRegistry::instance().listAll()) {
      AgentRegistry::instance().unregisterAgent(agentId);
    }

    ConfigLoader::instance().updateConfig(originalConfig_);

    if (!originalHome_.empty())
      setenv("HOME", originalHome_.c_str(), 1);
    else
      unsetenv("HOME");

    if (!originalPromptsDir_.empty())
      setenv("FIRMIUS_PROMPTS_DIR", originalPromptsDir_.c_str(), 1);
    else
      unsetenv("FIRMIUS_PROMPTS_DIR");

    if (!originalWorkflowsDir_.empty())
      setenv("FIRMIUS_WORKFLOWS_DIR", originalWorkflowsDir_.c_str(), 1);
    else
      unsetenv("FIRMIUS_WORKFLOWS_DIR");

    if (!originalHooksDir_.empty())
      setenv("FIRMIUS_HOOKS_DIR", originalHooksDir_.c_str(), 1);
    else
      unsetenv("FIRMIUS_HOOKS_DIR");

    unsetenv("FIRMIUS_HOME");
    std::filesystem::remove_all(testHome_);
  }

  class OneStopProvider final : public IProvider {
  public:
    explicit OneStopProvider(std::string id) : id_(std::move(id)) {}

    std::string getId() const override { return id_; }

    void stream(const AgentHistory &, const ProviderOptions &, 
                std::function<void(const StreamEvent &)> onEvent) override {
      const int count = streamCount_.fetch_add(1) + 1;
      std::string text;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (!responses_.empty()) {
          const std::size_t idx =
              static_cast<std::size_t>(std::min<int>(count - 1, responses_.size() - 1));
          text = responses_[idx];
        }
      }
      if (text.empty()) {
        text = count == 1 ? "first stop" : "second stop";
      }
      onEvent(TextChunk{text});
      onEvent(StreamDone{StopReason::Stop});
    }

    std::vector<ModelInfo> listModels() override {
      ModelInfo m;
      m.id = id_ + "-model";
      m.provider = id_;
      m.contextWindow = 4096;
      return {m};
    }

    ModelInfo getModelInfo(const std::string &) override { return listModels().front(); }

    void generateSummary(const std::string &, const AgentHistory &, const std::string &, 
                         std::function<void(const StreamEvent &)> onEvent,
                         std::atomic<bool> *) override {
      onEvent(TextChunk{"summary"});
      onEvent(StreamDone{StopReason::Stop});
    }

    ProviderType getProviderType() const override { return ProviderType::APIKey; }
    int streamCount() const { return streamCount_.load(); }
    void setResponses(std::vector<std::string> responses) {
      std::lock_guard<std::mutex> lock(mu_);
      responses_ = std::move(responses);
      streamCount_.store(0);
    }

  private:
    std::string id_;
    std::atomic<int> streamCount_{0};
    mutable std::mutex mu_;
    std::vector<std::string> responses_;
  };

  std::filesystem::path testHome_;
  std::filesystem::path testPromptsDir_;
  std::filesystem::path testWorkflowsDir_;
  std::filesystem::path testHooksDir_;
  std::string originalHome_;
  std::string originalPromptsDir_;
  std::string originalWorkflowsDir_;
  std::string originalHooksDir_;
  UserConfig originalConfig_;
  std::shared_ptr<OneStopProvider> provider_;
};

TEST_F(AgentStopHookTest, AgentStopHookFiresAndInjectsReminderTurn) {
  ThreadMetadata metadata;
  metadata.title = "AgentStop hook";
  metadata.hostOptions.type = HostType::Local;
  metadata.hostIdentifier = "localhost";
  metadata.cwd = "/tmp";
  metadata.leadPersona = "coder";

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const std::string threadId = tm.createThread(metadata);

  const std::string agentId = Engine::instance().summonAgent(threadId, "coder", "hello");

  ASSERT_TRUE(waitForCondition([&]() {
    auto ag = AgentRegistry::instance().getAgent(agentId);
    return ag != nullptr;
  })) << "agent not registered";

  ASSERT_TRUE(waitForCondition([&]() {
    auto ag = AgentRegistry::instance().getAgent(agentId);
    return ag && !ag->isRunning() && !ag->isBooting();
  })) << "agent did not stop";

  auto ag = AgentRegistry::instance().getAgent(agentId);
  ASSERT_NE(ag, nullptr);

  const auto &turns = ag->getContext().history->turns;
  bool sawHookReminder = false;
  for (const auto &turn : turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        if (const auto *txt = std::get_if<TextContent>(&part)) {
          if (txt->text.find("AGENT_STOP_FIRED") != std::string::npos) {
            sawHookReminder = true;
          }
        }
      }
    }
  }

  EXPECT_TRUE(sawHookReminder);
}

TEST_F(AgentStopHookTest, BlockingAgentStopHookContinuesAgentUntilAllowed) {
  if (!hooks::ScriptRuntime::enabled()) {
    GTEST_SKIP() << "Luau hooks disabled in this build";
  }

  std::ofstream hookFile(testWorkflowsDir_ / "agent_stop_hook.md");
  hookFile << R"(---
name: Agent Stop Gate
trigger:
  on_event: agent_stop
  block: true
action:
  kind: script
  body: "local iter = state.read('thread', 'promise.iteration') or 0; if iter == 0 then state.write('thread', 'promise.iteration', 1); return outcome.block{ reminder = 'DENY_STOP' } end; return outcome.allow{}"
---
)";
  hookFile.close();
  WorkflowLoader::instance().init();
  hooks::HookRegistry::instance().reload();

  ThreadMetadata metadata;
  metadata.title = "AgentStop gate";
  metadata.hostOptions.type = HostType::Local;
  metadata.hostIdentifier = "localhost";
  metadata.cwd = "/tmp";
  metadata.leadPersona = "coder";

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const std::string threadId = tm.createThread(metadata);
  hooks::HookState::instance().bindThread(threadId);
  ASSERT_TRUE(hooks::HookState::instance().writeJson(
      hooks::HookState::Scope::Thread, "promise.iteration", "0"));

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "hello");
  ASSERT_TRUE(waitForCondition([&]() {
    auto ag = AgentRegistry::instance().getAgent(agentId);
    return ag && !ag->isRunning() && !ag->isBooting();
  })) << "agent did not stop";

  EXPECT_GE(provider_->streamCount(), 2);
  auto ag = AgentRegistry::instance().getAgent(agentId);
  ASSERT_NE(ag, nullptr);
  bool sawGateReminder = false;
  for (const auto &turn : ag->getContext().history->turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        if (const auto *txt = std::get_if<TextContent>(&part);
            txt && txt->text.find("DENY_STOP") != std::string::npos) {
          sawGateReminder = true;
        }
      }
    }
  }
  EXPECT_TRUE(sawGateReminder);
}

TEST_F(AgentStopHookTest, ExternalPromiseHookSpawnsShrikeAndLoopsUntilSealed) {
  if (!hooks::ScriptRuntime::enabled()) {
    GTEST_SKIP() << "Luau hooks disabled in this build";
  }

  const auto packDir = testHooksDir_ / "promise";
  std::filesystem::create_directories(packDir / "flows");
  std::filesystem::create_directories(packDir / "scripts");

  std::ofstream flowFile(packDir / "flows" / "agent-stop-with-open-promise.yaml");
  flowFile << R"(id: promise.flow.agent-stop-with-open-promise
trigger:
  on_event: agent_stop
  match:
    state.thread.promise.state: { equals: open }
    state.thread.promise.id: { present: true }
  block: true
action:
  kind: script
  language: luau
  script_file: ../scripts/agent_stop_validator.lua
)";
  flowFile.close();

  std::ofstream scriptFile(packDir / "scripts" / "agent_stop_validator.lua");
  scriptFile << R"(local promise = state.read("thread", "promise")
if type(promise) ~= "table" or promise.state ~= "open" then
  return outcome.allow({})
end
local payload = event.payload or {}
if promise.agent_id and promise.agent_id ~= "" and promise.agent_id ~= payload.agent_id then
  return outcome.allow({})
end
local iteration = tonumber(promise.iteration) or 0
local next_iteration = iteration + 1
state.write("thread", "promise.state", "validating")
state.write("thread", "promise.iteration", next_iteration)
local log = thread.log_summary() or {}
local final_message = log.final_message or ""
local result = agent.spawn("shrike", "Validate promise " .. tostring(promise.id) .. "\nFinal: " .. final_message, { timeout_sec = 10 })
local verdict = "reject"
local suggestion = result and result.text or "no validator result"
if type(result) == "table" and type(result.json) == "table" then
  local parsed = result.json
  if type(parsed.verdict) == "table" and type(parsed.verdict.kind) == "string" then
    verdict = parsed.verdict.kind
  end
  if type(parsed.suggestion) == "string" then
    suggestion = parsed.suggestion
  end
end
state.append("thread", "promise.history[]", {
  iteration = next_iteration,
  verdict = verdict,
  suggestion = suggestion,
  validator_agent_id = result and result.agent_id or ""
})
if verdict == "accept" then
  state.write("thread", "promise.state", "sealed")
  state.write("thread", "promise.sealed_by", "shrike")
  return outcome.allow({ text = "PROMISE SEALED" })
end
state.write("thread", "promise.state", "open")
return outcome.block({
  reason = "promise rejected",
  reminder = "PROMISE DENIED\n" .. suggestion
})
)";
  scriptFile.close();

  WorkflowLoader::instance().init();
  hooks::HookRegistry::instance().reload();

  provider_->setResponses({
      "parent tried to stop before completing the promise",
      R"({"verdict":{"kind":"reject"},"suggestion":"Finish the promised work first."})",
      "parent completed the promised work after Shrike notes",
      R"({"verdict":{"kind":"accept"},"suggestion":"Evidence is sufficient."})",
  });

  ThreadMetadata metadata;
  metadata.title = "Promise stop gate";
  metadata.hostOptions.type = HostType::Local;
  metadata.hostIdentifier = "localhost";
  metadata.cwd = "/tmp";
  metadata.leadPersona = "coder";

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const std::string threadId = tm.createThread(metadata);
  const std::string promisedAgentId = "promised-agent";

  hooks::HookState::instance().bindThread(threadId);
  ASSERT_TRUE(hooks::HookState::instance().applyBatch(
      {
          {hooks::HookState::Scope::Thread, "promise.id", R"("pact-1")", false},
          {hooks::HookState::Scope::Thread, "promise.task",
           R"("finish the promised task")", false},
          {hooks::HookState::Scope::Thread, "promise.brief",
           R"("finish the promised task")", false},
          {hooks::HookState::Scope::Thread, "promise.done_when",
           R"(["work is complete"])", false},
          {hooks::HookState::Scope::Thread, "promise.validator", R"("shrike")",
           false},
          {hooks::HookState::Scope::Thread, "promise.iteration", "0", false},
          {hooks::HookState::Scope::Thread, "promise.max_iterations", "5",
           false},
          {hooks::HookState::Scope::Thread, "promise.state", R"("open")",
           false},
          {hooks::HookState::Scope::Thread, "promise.agent_id",
           R"("promised-agent")", false},
          {hooks::HookState::Scope::Thread, "promise.history", "[]", false},
      },
      "promise-test"));

  const std::string agentId = Engine::instance().summonAgent(
      threadId, "coder", "start promise task", true, "", "coder", "",
      promisedAgentId);
  ASSERT_EQ(agentId, promisedAgentId);

  ASSERT_TRUE(waitForCondition([&]() {
    auto ag = AgentRegistry::instance().getAgent(agentId);
    return ag && !ag->isRunning() && !ag->isBooting() &&
           provider_->streamCount() >= 4;
  }, std::chrono::milliseconds(10000))) << "promise gate did not settle";

  auto promiseJson = hooks::HookState::instance().readJson(
      hooks::HookState::Scope::Thread, "promise", "promise-test");
  ASSERT_TRUE(promiseJson.has_value());
  rapidjson::Document promise;
  ASSERT_FALSE(promise.Parse(promiseJson->c_str()).HasParseError());
  ASSERT_TRUE(promise.IsObject());
  ASSERT_TRUE(promise.HasMember("state"));
  ASSERT_TRUE(promise["state"].IsString());
  EXPECT_STREQ(promise["state"].GetString(), "sealed");
  ASSERT_TRUE(promise.HasMember("iteration"));
  ASSERT_TRUE(promise["iteration"].IsNumber());
  EXPECT_EQ(static_cast<int>(promise["iteration"].GetDouble()), 2);
  ASSERT_TRUE(promise.HasMember("history"));
  ASSERT_TRUE(promise["history"].IsArray());
  ASSERT_EQ(promise["history"].Size(), 2u);

  auto ag = AgentRegistry::instance().getAgent(agentId);
  ASSERT_NE(ag, nullptr);
  bool sawDenyReminder = false;
  for (const auto &turn : ag->getContext().history->turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        if (const auto *txt = std::get_if<TextContent>(&part);
            txt && txt->text.find("PROMISE DENIED") != std::string::npos) {
          sawDenyReminder = true;
        }
      }
    }
  }
  EXPECT_TRUE(sawDenyReminder);
}

} // namespace
