#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "workflow/WorkflowLoader.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "persistence/ThreadManager.hpp"
#include "Message.hpp"
#include "ConfigLoader.hpp"
#include "providers/ProviderRegistry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;

namespace {

template <typename Fn>
bool waitForCondition(Fn &&fn,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return fn();
}

class HookAgentActionE2ETest : public ::testing::Test {
protected:
  void SetUp() override {
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_hook_agent_e2e_" +
                 std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(testHome_);
    std::filesystem::create_directories(testHome_ / ".firmius" / "threads");
    std::filesystem::create_directories(testHome_ / "workflows");
    setenv("HOME", testHome_.c_str(), 1);
    setenv("FIRMIUS_HOME", testHome_.c_str(), 1);
    setenv("FIRMIUS_WORKFLOWS_DIR", (testHome_ / "workflows").c_str(), 1);
    originalConfig_ = ConfigLoader::instance().getConfig();

    auto sourcePath = std::filesystem::path(__FILE__);
    auto repoRoot = sourcePath.parent_path().parent_path().parent_path().parent_path();
    setenv("FIRMIUS_PROMPTS_DIR", (repoRoot / "prompts").c_str(), 1);

    std::ofstream hookFile(testHome_ / "workflows" / "agent_hook.md");
    hookFile << R"(---
name: Agent Hook
trigger:
  on_event: pre_tool_use
  match:
    tool: Process
action:
  kind: agent
  target_persona: aster
  agent_task: Say exactly: hook child success
emit:
  outcome: "{{subagent.return.kind}}"
---
Hook body
)";
    hookFile.close();

    provider_ = std::make_shared<OneShotProvider>("hook-agent-provider");
    ProviderRegistry::instance().registerProvider(provider_);
    auto cfg = ConfigLoader::instance().getConfig();
    cfg.defaultProviderId = provider_->getId();
    cfg.defaultModelId = provider_->listModels().front().id;
    ConfigLoader::instance().updateConfig(cfg);

    WorkflowLoader::instance().init();
    hooks::HookRegistry::instance().reload();
  }

  void TearDown() override {
    for (const auto &agentId : AgentRegistry::instance().listAll()) {
      Engine::instance().terminateAgent(agentId);
    }
    ConfigLoader::instance().updateConfig(originalConfig_);
    unsetenv("FIRMIUS_PROMPTS_DIR");
    unsetenv("FIRMIUS_WORKFLOWS_DIR");
    unsetenv("FIRMIUS_HOME");
    std::filesystem::remove_all(testHome_);
  }

  class OneShotProvider final : public IProvider {
  public:
    explicit OneShotProvider(std::string id) : id_(std::move(id)) {}
    std::string getId() const override { return id_; }
    void stream(const AgentHistory &, const ProviderOptions &,
                std::function<void(const StreamEvent &)> onEvent) override {
      onEvent(TextChunk{"hook child success"});
      onEvent(StreamDone{StopReason::Stop});
    }
    std::vector<ModelInfo> listModels() override {
      ModelInfo model;
      model.id = id_ + "-model";
      model.provider = id_;
      model.contextWindow = 4096;
      return {model};
    }
    ModelInfo getModelInfo(const std::string &) override {
      return listModels().front();
    }
    void generateSummary(const std::string &, const AgentHistory &,
                         const std::string &,
                         std::function<void(const StreamEvent &)> onEvent,
                         std::atomic<bool> *) override {
      onEvent(TextChunk{"hook child success"});
      onEvent(StreamDone{StopReason::Stop});
    }
    ProviderType getProviderType() const override { return ProviderType::APIKey; }

  private:
    std::string id_;
  };

  std::filesystem::path testHome_;
  UserConfig originalConfig_;
  std::shared_ptr<OneShotProvider> provider_;
};

TEST_F(HookAgentActionE2ETest, PreToolUseAgentActionSpawnsAgentAndReturnsOutcome) {
  ThreadMetadata metadata;
  metadata.title = "Hook Agent Action";
  metadata.hostOptions.type = HostType::Local;
  metadata.hostIdentifier = "localhost";
  metadata.cwd = "/tmp";
  metadata.leadPersona = "lead";

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const std::string threadId = tm.createThread(metadata);

  hooks::EventPayload payload;
  payload.threadId = threadId;
  payload.agentId = "parent-agent";
  payload.persona = "aster";
  payload.activeMode = "execute";
  payload.toolName = "Process";
  payload.toolArgsJson = R"({"action":"Execute","command":"echo ignored","cwd":"/tmp","timeout_ms":1000})";

  auto result = hooks::HookDispatcher::fire(WorkflowEventKind::PreToolUse, payload);
  ASSERT_TRUE(result.firstOutcome.has_value());
  ASSERT_FALSE(result.firstOutcome->spawnedAgentId.empty());
  EXPECT_FALSE(result.firstOutcome->outcomeLabel.empty());
  EXPECT_FALSE(result.firstOutcome->spawnedAgentReturnJson.empty());

  const std::string childId = result.firstOutcome->spawnedAgentId;
  ASSERT_TRUE(waitForCondition([&]() {
    auto child = AgentRegistry::instance().getAgent(childId);
    return child && !child->isRunning() && !child->isBooting();
  }));

  auto child = AgentRegistry::instance().getAgent(childId);
  ASSERT_NE(child, nullptr);
  std::optional<AgentOutcome> outcome;
  ASSERT_TRUE(waitForCondition([&]() {
    outcome = Engine::instance().peekAgentOutcome(childId, std::chrono::milliseconds(25));
    return outcome.has_value();
  }));
  ASSERT_TRUE(outcome.has_value());
  EXPECT_NE(outcome->kind, AgentOutcome::Kind::Cancelled);
  EXPECT_FALSE(outcome->text.empty());
}

} // namespace
