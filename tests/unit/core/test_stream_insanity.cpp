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

enum class InsanityMode {
  Normal,           // Normal content, no issues
  RepetitiveLoop,   // Same phrase repeated over and over
  RandomJunk,       // High-entropy gibberish
  ExcessiveTokens,  // Streams until token limit exceeded
};

class InsanityProvider : public IProvider {
public:
  InsanityProvider(std::string providerId, InsanityMode mode,
                   const std::string &pattern = "loop")
      : providerId_(std::move(providerId)), mode_(mode), pattern_(pattern) {}

  std::string getId() const override { return providerId_; }

  void stream(const AgentHistory &, const ProviderOptions &opts,
              std::function<void(const StreamEvent &)> onEvent) override {
    callCount_.fetch_add(1);

    switch (mode_) {
    case InsanityMode::Normal: {
      // Normal single response
      onEvent(TextChunk{"This is a normal response."});
      onEvent(StreamDone{StopReason::Stop});
      return;
    }

    case InsanityMode::RepetitiveLoop: {
      // After first call, produce normal content (simulating recovery after intervention)
      if (callCount_.load() > 1) {
        onEvent(TextChunk{"This is a normal response after intervention."});
        onEvent(StreamDone{StopReason::Stop});
        return;
      }
      // Stream the same phrase 50+ times to trigger repetition detection
      const std::string phrase = " " + pattern_ + " ";
      for (int i = 0; i < 50; ++i) {
        if (opts.abortSignal && opts.abortSignal->load()) {
          return;
        }
        onEvent(TextChunk{phrase});
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      onEvent(StreamDone{StopReason::Stop});
      return;
    }

    case InsanityMode::RandomJunk: {
      // Stream high-entropy random characters
      // This creates gibberish with high entropy
      std::string junk;
      for (int i = 0; i < 100; ++i) {
        if (opts.abortSignal && opts.abortSignal->load()) {
          return;
        }
        // Generate random printable ASCII but in a way that looks like high entropy
        for (int j = 0; j < 50; ++j) {
          char c = 33 + (rand() % 94);  // Printable ASCII
          junk.push_back(c);
        }
        onEvent(TextChunk{junk});
        junk.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      onEvent(StreamDone{StopReason::Stop});
      return;
    }

    case InsanityMode::ExcessiveTokens: {
      // After first call, produce normal content (simulating recovery after intervention)
      if (callCount_.load() > 1) {
        onEvent(TextChunk{"This is a normal response after intervention."});
        onEvent(StreamDone{StopReason::Stop});
        return;
      }
      // Stream massive amounts of text to exceed token limit
      // Default limit is 50000 tokens, so stream 60000+ tokens
      std::string massive;
      for (int i = 0; i < 60000; ++i) {
        if (opts.abortSignal && opts.abortSignal->load()) {
          return;
        }
        massive += "word ";
        if (massive.size() > 200) {
          onEvent(TextChunk{massive});
          massive.clear();
        }
      }
      if (!massive.empty()) {
        onEvent(TextChunk{massive});
      }
      onEvent(StreamDone{StopReason::Stop});
      return;
    }
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

  int getCallCount() const { return callCount_.load(); }

private:
  std::string providerId_;
  InsanityMode mode_;
  std::string pattern_;
  std::atomic<int> callCount_{0};
};

class StreamInsanityTest : public ::testing::Test {
protected:
  void SetUp() override {
    Panic::init();
    originalHome_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    originalPromptsDir_ = std::getenv("FIRMIUS_PROMPTS_DIR")
                              ? std::getenv("FIRMIUS_PROMPTS_DIR")
                              : "";
    originalConfig_ = ConfigLoader::instance().getConfig();

    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_insanity_home_" +
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
    metadata.title = "Insanity Test";
    metadata.cwd = testHome_.string();
    metadata.hostOptions.type = HostType::Local;
    metadata.leadPersona = "lead";
    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    return tm.createThread(metadata);
  }

  std::shared_ptr<InsanityProvider> registerProvider(InsanityMode mode,
                                                      const std::string &pattern = "") {
    const std::string providerId =
        "insanity-provider-" +
        std::to_string(static_cast<long long>(
            providerSerial_.fetch_add(1, std::memory_order_relaxed)));
    auto provider = std::make_shared<InsanityProvider>(providerId, mode, pattern);
    ProviderRegistry::instance().registerProvider(provider);

    auto cfg = ConfigLoader::instance().getConfig();
    cfg.defaultProviderId = provider->getId();
    cfg.defaultModelId = provider->listModels().front().id;
    // Enable insanity detection with conservative thresholds for tests
    cfg.insanityDetectionEnabled = true;
    cfg.insanityRepetitionThreshold = 3;   // Flag after 3 repeats
    cfg.insanityMaxTokenThreshold = 1000;  // Low threshold for test
    cfg.maxInsanityRetries = 2;
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
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
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

// Test: Normal content should complete without intervention
TEST_F(StreamInsanityTest, NormalContentCompletesSuccessfully) {
  auto provider = registerProvider(InsanityMode::Normal);
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "say hello");

  ASSERT_TRUE(waitForAgentStarted(agentId));

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Response);
  EXPECT_NE(outcome->text.find("normal response"), std::string::npos);
  // Should only have called provider once (no retries)
  EXPECT_EQ(provider->getCallCount(), 1);
}

// Test: Repetitive content triggers detection and retry
TEST_F(StreamInsanityTest, RepetitiveContentTriggersDetectionAndRetry) {
  auto provider = registerProvider(InsanityMode::RepetitiveLoop, "LOOP");
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "do something");

  ASSERT_TRUE(waitForAgentStarted(agentId));

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());

  // Should have retried (provider called multiple times)
  // The detector should have intervened on first attempt
  EXPECT_GT(provider->getCallCount(), 1);

  // Verify the outcome is successful (retry worked)
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Response);

  // Verify history doesn't contain the repetitive junk
  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const AgentHistory persisted = tm.loadAgentHistory(threadId, agentId);

  // Check that no turn contains excessive "LOOP" repetition
  for (const auto &turn : persisted.turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == Role::Assistant) {
        for (const auto &part : msg.content) {
          if (auto *txt = std::get_if<TextContent>(&part)) {
            // The text should not be primarily repetitive junk
            int loopCount = 0;
            std::size_t pos = 0;
            while ((pos = txt->text.find("LOOP", pos)) != std::string::npos) {
              loopCount++;
              pos += 4;
            }
            // If there are many LOOP occurrences, that's a failure
            EXPECT_LE(loopCount, 5) << "Found excessive LOOP repetitions in turn "
                                    << turn.turnId;
          }
        }
      }
    }
  }
}

// Test: Excessive token count triggers detection and retry
TEST_F(StreamInsanityTest, ExcessiveTokenCountTriggersDetection) {
  auto provider = registerProvider(InsanityMode::ExcessiveTokens);
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "write a lot");

  ASSERT_TRUE(waitForAgentStarted(agentId));

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());

  // Should have retried due to excessive tokens
  EXPECT_GT(provider->getCallCount(), 1);
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Response);
}

// Test: Insanity detection can be disabled via config
TEST_F(StreamInsanityTest, DetectionCanBeDisabled) {
  auto provider = registerProvider(InsanityMode::RepetitiveLoop, "NOPE");
  
  // Disable detection AFTER registering provider (registerProvider enables it)
  auto cfg = ConfigLoader::instance().getConfig();
  cfg.insanityDetectionEnabled = false;
  ConfigLoader::instance().updateConfig(cfg);

  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "loop forever");

  ASSERT_TRUE(waitForAgentStarted(agentId));

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());

  // With detection disabled, should only call provider once
  EXPECT_EQ(provider->getCallCount(), 1);
}

// Test: Intervention nudge is added to history when insanity detected
TEST_F(StreamInsanityTest, InterventionNudgeAddedToHistory) {
  auto provider = registerProvider(InsanityMode::RepetitiveLoop, "INSANE");
  const std::string threadId = createThread();

  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "repeat test");

  ASSERT_TRUE(waitForAgentStarted(agentId));

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());

  // Check history for intervention nudge
  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const AgentHistory persisted = tm.loadAgentHistory(threadId, agentId);

  bool sawIntervention = false;
  for (const auto &turn : persisted.turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == Role::System) {
        for (const auto &part : msg.content) {
          if (auto *txt = std::get_if<TextContent>(&part)) {
            if (txt->text.find("insanity") != std::string::npos ||
                txt->text.find("repeating") != std::string::npos ||
                txt->text.find("stop") != std::string::npos) {
              sawIntervention = true;
              break;
            }
          }
        }
      }
    }
  }

  // We should have seen an intervention nudge
  EXPECT_TRUE(sawIntervention) << "Expected intervention nudge in history";
}

} // namespace
