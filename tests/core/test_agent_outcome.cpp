#include "ConfigLoader.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "Panic.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/InterruptibleSleep.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <thread>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;

namespace {

enum class OutcomeMode {
  Response,
  DelayedResponse,
  NoSummary,
  Cancelled,
  ProviderRetrySleep,
  Failed,
};

class OutcomeProvider : public IProvider {
public:
  OutcomeProvider(std::string providerId, OutcomeMode mode, std::string text = {})
      : providerId_(std::move(providerId)), mode_(mode), text_(std::move(text)) {}

  std::string getId() const override { return providerId_; }

  void stream(const AgentHistory &, const ProviderOptions &opts,
              std::function<void(const StreamEvent &)> onEvent) override {
    callCount_.fetch_add(1);
    switch (mode_) {
    case OutcomeMode::Response:
      onEvent(TextChunk{text_});
      onEvent(StreamDone{StopReason::Stop});
      return;
    case OutcomeMode::DelayedResponse:
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      onEvent(TextChunk{text_});
      onEvent(StreamDone{StopReason::Stop});
      return;
    case OutcomeMode::NoSummary:
      onEvent(StreamDone{StopReason::Stop});
      return;
    case OutcomeMode::Failed:
      throw std::runtime_error(text_.empty() ? "boom" : text_);
    case OutcomeMode::Cancelled:
      while (!opts.abortSignal || !opts.abortSignal->load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      onEvent(StreamDone{StopReason::Stop});
      return;
    case OutcomeMode::ProviderRetrySleep:
      onEvent(StreamRetrying{1, 3, 429, 5000, "Provider retry wait", "", ""});
      {
        std::lock_guard<std::mutex> lock(mutex_);
        enteredRetrySleep_ = true;
      }
      retrySleepCv_.notify_all();
      if (!firmius::shared::interruptibleSleep(std::chrono::seconds(5),
                                               opts.abortController,
                                               opts.abortSignal)) {
        return;
      }
      onEvent(StreamDone{StopReason::Stop});
      return;
    }
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = providerId_ + "-model";
    model.provider = providerId_;
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }

  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> *abortSignal = nullptr) override {
    ProviderOptions opts;
    opts.abortSignal = abortSignal;
    stream(AgentHistory{}, opts, std::move(onEvent));
  }

  ProviderType getProviderType() const override {
    return ProviderType::APIKey;
  }

  bool waitUntilRetrySleep(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(1500)) {
    std::unique_lock<std::mutex> lock(mutex_);
    return retrySleepCv_.wait_for(lock, timeout,
                                  [this]() { return enteredRetrySleep_; });
  }

private:
  std::string providerId_;
  OutcomeMode mode_;
  std::string text_;
  std::atomic<int> callCount_{0};
  std::mutex mutex_;
  std::condition_variable retrySleepCv_;
  bool enteredRetrySleep_ = false;
};

class HistoryCaptureProvider : public IProvider {
public:
  explicit HistoryCaptureProvider(std::string providerId)
      : providerId_(std::move(providerId)) {}

  std::string getId() const override { return providerId_; }

  void stream(const AgentHistory &history, const ProviderOptions &,
              std::function<void(const StreamEvent &)> onEvent) override {
    lastHistory_ = history;
    onEvent(TextChunk{"captured"});
    onEvent(StreamDone{StopReason::Stop});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = providerId_ + "-model";
    model.provider = providerId_;
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }

  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> * = nullptr) override {
    onEvent(TextChunk{"summary"});
    onEvent(StreamDone{StopReason::Stop});
  }

  ProviderType getProviderType() const override {
    return ProviderType::APIKey;
  }

  const AgentHistory &lastHistory() const { return lastHistory_; }

private:
  std::string providerId_;
  AgentHistory lastHistory_;
};

class AgentOutcomeTest : public ::testing::Test {
protected:
  void SetUp() override {
    Panic::init();
    originalHome_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    originalPromptsDir_ = std::getenv("FIRMIUS_PROMPTS_DIR")
                              ? std::getenv("FIRMIUS_PROMPTS_DIR")
                              : "";
    originalConfig_ = ConfigLoader::instance().getConfig();

    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_agent_outcome_home_" +
                 std::to_string(static_cast<long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())));
    testPromptsDir_ = testHome_ / "prompts";
    std::filesystem::create_directories(testHome_ / ".firmius" / "threads");
    std::filesystem::create_directories(testPromptsDir_);
    setenv("HOME", testHome_.c_str(), 1);
    setenv("FIRMIUS_PROMPTS_DIR", testPromptsDir_.c_str(), 1);

    std::ofstream coderFile(testPromptsDir_ / "coder.md");
    coderFile << "---\nname: coder\ntitle: Coder\n---\nCoder identity";
    coderFile.close();
  }

  void TearDown() override {
    Engine::instance().shutdown();
    for (const auto &agentId : AgentRegistry::instance().listAll()) {
      AgentRegistry::instance().unregisterAgent(agentId);
    }
    ConfigLoader::instance().updateConfig(originalConfig_);
    std::filesystem::remove_all(testHome_);
    if (originalHome_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", originalHome_.c_str(), 1);
    }
    if (originalPromptsDir_.empty()) {
      unsetenv("FIRMIUS_PROMPTS_DIR");
    } else {
      setenv("FIRMIUS_PROMPTS_DIR", originalPromptsDir_.c_str(), 1);
    }
  }

  std::string createThread() {
    ThreadMetadata metadata;
    metadata.title = "Agent Outcome Test";
    metadata.cwd = testHome_.string();
    metadata.hostOptions.type = HostType::Local;
    metadata.leadPersona = "lead";
    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    return tm.createThread(metadata);
  }

  std::string createPlanWithAssignedChunk(const std::string &threadId,
                                          const std::string &agentId,
                                          WorkChunkStatus status) {
    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    Plan plan;
    plan.threadId = threadId;
    plan.title = "Assigned chunk test";
    plan.objective = "Verify ownership release";
    plan.context = "Test context";
    plan.strategy = "Test strategy";
    plan.status = PlanStatus::Active;
    const std::string planId = tm.createPlan(plan);

    auto persisted = tm.getPlan(threadId, planId);
    WorkChunk chunk;
    chunk.id = "chunk-1";
    chunk.title = "chunk";
    chunk.goal = "goal";
    chunk.context = "context";
    chunk.constraints = "constraints";
    chunk.completion = "completion";
    chunk.status = status;
    chunk.assignedAgentId = agentId;
    chunk.createdAt = 1;
    chunk.updatedAt = 1;
    persisted.chunks.push_back(chunk);
    tm.updatePlan(threadId, persisted);
    return planId;
  }

  std::shared_ptr<OutcomeProvider> registerProvider(OutcomeMode mode,
                                                    const std::string &text = {}) {
    const std::string providerId =
        "agent-outcome-provider-" +
        std::to_string(static_cast<long long>(
            providerSerial_.fetch_add(1, std::memory_order_relaxed)));
    auto provider = std::make_shared<OutcomeProvider>(providerId, mode, text);
    ProviderRegistry::instance().registerProvider(provider);

    auto cfg = ConfigLoader::instance().getConfig();
    cfg.defaultProviderId = provider->getId();
    cfg.defaultModelId = provider->listModels().front().id;
    ConfigLoader::instance().updateConfig(cfg);
    return provider;
  }

  static bool waitForAgentStarted(const std::string &agentId) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto active = Engine::instance().listActiveAgents();
      if (std::find(active.begin(), active.end(), agentId) != active.end()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  static std::optional<AgentOutcome> waitForOutcome(const std::string &agentId) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      auto outcome = Engine::instance().waitForAgentOutcome(
          agentId, std::chrono::milliseconds(50));
      if (outcome.has_value()) {
        return outcome;
      }
    }
    return std::nullopt;
  }

  std::filesystem::path testHome_;
  std::filesystem::path testPromptsDir_;
  std::string originalHome_;
  std::string originalPromptsDir_;
  UserConfig originalConfig_;
  static inline std::atomic<int> providerSerial_{0};
};

TEST_F(AgentOutcomeTest, CompletedWithTextProducesResponseOutcome) {
  registerProvider(OutcomeMode::Response, "final response");
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "say hello");

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Response);
  EXPECT_EQ(outcome->text, "final response");
}

TEST_F(AgentOutcomeTest, OversizedAssistantTextIsClampedBeforePersistence) {
  std::string huge = "Large response heading.\n";
  for (int i = 0; i < 8000; ++i) {
    huge += "Repeated response line " + std::to_string(i) + "\n";
  }

  registerProvider(OutcomeMode::Response, huge);
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "say hello");

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Response);
  EXPECT_LT(outcome->text.size(), huge.size());
  EXPECT_NE(outcome->text.find("Assistant response truncated for persistence"),
            std::string::npos);

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const AgentHistory persisted = tm.loadAgentHistory(threadId, agentId);
  ASSERT_FALSE(persisted.turns.empty());

  std::string persistedText;
  for (auto it = persisted.turns.rbegin(); it != persisted.turns.rend(); ++it) {
    for (const auto &msg : it->messages) {
      if (msg.role != Role::Assistant) {
        continue;
      }
      for (const auto &part : msg.content) {
        if (auto *txt = std::get_if<TextContent>(&part)) {
          persistedText = txt->text;
          break;
        }
      }
      if (!persistedText.empty()) {
        break;
      }
    }
    if (!persistedText.empty()) {
      break;
    }
  }

  ASSERT_FALSE(persistedText.empty());
  EXPECT_LT(persistedText.size(), huge.size());
  EXPECT_NE(persistedText.find("Assistant response truncated for persistence"),
            std::string::npos);
}

TEST_F(AgentOutcomeTest, CompletedAgentReleasesOwnedChunkAssignment) {
  registerProvider(OutcomeMode::DelayedResponse, "final response");
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "say hello");
  ASSERT_TRUE(waitForAgentStarted(agentId));
  const std::string planId =
      createPlanWithAssignedChunk(threadId, agentId, WorkChunkStatus::InProgress);

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Response);

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const auto plan = tm.getPlan(threadId, planId);
  ASSERT_EQ(plan.chunks.size(), 1u);
  EXPECT_TRUE(plan.chunks[0].assignedAgentId.empty());
  EXPECT_EQ(plan.chunks[0].status, WorkChunkStatus::InProgress);
}

TEST_F(AgentOutcomeTest, CancelledAgentReleasesOwnedChunkAndReadiesIt) {
  registerProvider(OutcomeMode::Cancelled);
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "wait for cancel");
  ASSERT_TRUE(waitForAgentStarted(agentId));
  const std::string planId =
      createPlanWithAssignedChunk(threadId, agentId, WorkChunkStatus::InProgress);

  Engine::instance().cancelAgent(agentId);
  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Cancelled);

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const auto plan = tm.getPlan(threadId, planId);
  ASSERT_EQ(plan.chunks.size(), 1u);
  EXPECT_TRUE(plan.chunks[0].assignedAgentId.empty());
  EXPECT_EQ(plan.chunks[0].status, WorkChunkStatus::Ready);
}

TEST_F(AgentOutcomeTest, CompletedWithoutTextProducesNoSummaryOutcome) {
  registerProvider(OutcomeMode::NoSummary);
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "say hello");

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::NoSummary);
  EXPECT_TRUE(outcome->text.empty());
}

TEST_F(AgentOutcomeTest, CancellationProducesCancelledOutcome) {
  registerProvider(OutcomeMode::Cancelled);
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "wait for cancel");

  ASSERT_TRUE(waitForAgentStarted(agentId));
  Engine::instance().cancelAgent(agentId);
  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Cancelled);
}

TEST_F(AgentOutcomeTest, RuntimeOverlaysAreSentToProviderButNotPersistedToJournal) {
  const std::string providerId =
      "history-capture-provider-" +
      std::to_string(static_cast<long long>(
          providerSerial_.fetch_add(1, std::memory_order_relaxed)));
  auto provider = std::make_shared<HistoryCaptureProvider>(providerId);
  ProviderRegistry::instance().registerProvider(provider);

  auto cfg = ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = provider->getId();
  cfg.defaultModelId = provider->listModels().front().id;
  ConfigLoader::instance().updateConfig(cfg);

  const std::string threadId = createThread();
  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "say hello");

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Response);

  bool sawRuntimeWorkOverlay = false;
  bool sawRuntimeWatchedOverlay = false;
  for (const auto &turn : provider->lastHistory().turns) {
    if (turn.turnId == "runtime-overlay-work-state") {
      sawRuntimeWorkOverlay = true;
    }
    if (turn.turnId == "runtime-overlay-watched-files") {
      sawRuntimeWatchedOverlay = true;
    }
  }
  EXPECT_TRUE(sawRuntimeWorkOverlay);
  EXPECT_TRUE(sawRuntimeWatchedOverlay);

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const AgentHistory persisted = tm.loadAgentHistory(threadId, agentId);
  for (const auto &turn : persisted.turns) {
    EXPECT_NE(turn.turnId, "runtime-overlay-work-state");
    EXPECT_NE(turn.turnId, "runtime-overlay-watched-files");
  }
}

TEST_F(AgentOutcomeTest, ProviderOwnedRetrySleepCancelsImmediately) {
  auto provider = registerProvider(OutcomeMode::ProviderRetrySleep);
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "wait for provider retry");

  ASSERT_TRUE(waitForAgentStarted(agentId));
  ASSERT_TRUE(provider->waitUntilRetrySleep());

  const auto cancelStart = std::chrono::steady_clock::now();
  Engine::instance().cancelAgent(agentId);
  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - cancelStart);
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Cancelled);
  EXPECT_LT(elapsed.count(), 1000);
}

TEST_F(AgentOutcomeTest, FailureProducesFailedOutcome) {
  auto provider = registerProvider(OutcomeMode::Failed, "boom");
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "fail now");

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Failed);
  EXPECT_NE(outcome->text.find("boom"), std::string::npos);
  EXPECT_NE(outcome->text.find("Provider: " + provider->getId()), std::string::npos);
}

TEST_F(AgentOutcomeTest, ResumeRestoresLoadedSkillsAndMds) {
  const std::string threadId = createThread();
  const std::string agentId = "resume-test-agent";

  // Define a trusted skills directory and path
  std::filesystem::path skillsDir = testHome_ / "skills";
  std::filesystem::path skillRoot = skillsDir / "test-skill";
  std::filesystem::path skillPath = skillRoot / "SKILL.md";
  std::filesystem::create_directories(skillRoot);
  setenv("FIRMIUS_SKILLS_DIR", skillsDir.string().c_str(), 1);

  // Write the skill file so it can be read during overlay generation
  std::ofstream skillFile(skillPath);
  skillFile << "Skill Content";
  skillFile.close();

  // 1. Seed persisted history with a successful skill_load result
  {
    Journaler journaler(threadId, agentId);
    AgentTurn callTurn;
    callTurn.turnId = "turn-1";
    callTurn.stopReason = StopReason::Stop;
    Message callMessage;
    callMessage.id = "msg-1";
    callMessage.role = Role::Assistant;
    callMessage.timestamp = 1700000000;
    ToolCallContent call;
    call.id = "call-1";
    call.name = "skill_load";
    call.args = R"({"id":"test-skill"})";
    callMessage.content.push_back(call);
    callTurn.messages.push_back(callMessage);
    journaler.appendTurn(callTurn);

    AgentTurn resultTurn;
    resultTurn.turnId = "turn-2";
    resultTurn.stopReason = StopReason::Stop;
    Message resultMessage;
    resultMessage.id = "msg-2";
    resultMessage.role = Role::ToolResult;
    resultMessage.timestamp = 1700000001;
    ToolResultContent result;
    result.toolCallId = "call-1";
    result.success = true;
    result.result = std::string("{\"skill_id\":\"test-skill\",\"path\":\"") +
                    skillPath.string() + "\",\"skill_root\":\"" +
                    skillRoot.string() + "\"}";
    resultMessage.content.push_back(result);
    resultTurn.messages.push_back(resultMessage);
    journaler.appendTurn(resultTurn);
  }

  // 2. Prepare provider to capture restored state
  const std::string providerId = "resume-capture-provider";
  auto provider = std::make_shared<HistoryCaptureProvider>(providerId);
  ProviderRegistry::instance().registerProvider(provider);

  auto cfg = ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = provider->getId();
  cfg.defaultModelId = provider->listModels().front().id;
  ConfigLoader::instance().updateConfig(cfg);

  // 3. Resume the agent
  Engine::instance().resumeAgent(threadId, agentId, "coder", "", "lead", "Coder", true);
  
  // 4. Verify state in memory
  auto agent = AgentRegistry::instance().getAgent(agentId);
  ASSERT_NE(agent, nullptr);
  
  const auto& state = agent->getContext().state;
  bool skillFound = std::find(state.loadedSkills.begin(), state.loadedSkills.end(), "test-skill") != state.loadedSkills.end();
  bool pathFound = std::find(state.loadedAgentMds.begin(), state.loadedAgentMds.end(), skillPath.string()) != state.loadedAgentMds.end();
  EXPECT_TRUE(skillFound);
  EXPECT_TRUE(pathFound);
  
  // 5. Verify state is sent to provider via RuntimeOverlay
  // Trigger a real follow-up task to see the overlay in action
  Engine::instance().executeTask(agentId, "Continue verification");

  // Wait for the provider to capture history (resumed agent starts working)
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline && provider->lastHistory().turns.empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_FALSE(provider->lastHistory().turns.empty());
  bool sawSkillsOverlay = false;
  for (const auto& turn : provider->lastHistory().turns) {
    if (turn.turnId == "runtime-overlay-loaded-skills") {
      sawSkillsOverlay = true;
      EXPECT_NE(std::get<TextContent>(turn.messages.front().content.front()).text.find("test-skill"), std::string::npos);
      EXPECT_NE(std::get<TextContent>(turn.messages.front().content.front()).text.find(skillPath.string()), std::string::npos);
      EXPECT_NE(std::get<TextContent>(turn.messages.front().content.front()).text.find("Skill Content"), std::string::npos);
    }
  }
  EXPECT_TRUE(sawSkillsOverlay);
}
} // namespace
