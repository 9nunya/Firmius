#include "ConfigLoader.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "Panic.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"

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
  NoSummary,
  Cancelled,
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

private:
  std::string providerId_;
  OutcomeMode mode_;
  std::string text_;
  std::atomic<int> callCount_{0};
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

TEST_F(AgentOutcomeTest, FailureProducesFailedOutcome) {
  registerProvider(OutcomeMode::Failed, "boom");
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "fail now");

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Failed);
  EXPECT_EQ(outcome->text, "boom");
}

} // namespace
