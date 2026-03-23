#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "Engine.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;

namespace {

template <typename Fn>
bool waitForCondition(Fn &&fn,
                      std::chrono::milliseconds timeout =
                          std::chrono::milliseconds(3000),
                      std::chrono::milliseconds step =
                          std::chrono::milliseconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) {
      return true;
    }
    std::this_thread::sleep_for(step);
  }
  return fn();
}

std::string latestUserText(const AgentHistory &history) {
  for (auto turnIt = history.turns.rbegin(); turnIt != history.turns.rend();
       ++turnIt) {
    for (auto msgIt = turnIt->messages.rbegin(); msgIt != turnIt->messages.rend();
         ++msgIt) {
      if (msgIt->role != Role::User) {
        continue;
      }
      std::string out;
      for (const auto &part : msgIt->content) {
        if (const auto *txt = std::get_if<TextContent>(&part)) {
          out += txt->text;
        }
      }
      if (!out.empty()) {
        return out;
      }
    }
  }
  return "";
}

class CaptureUserHistoryProvider : public IProvider {
public:
  std::string getId() const override { return "capture-user-history-provider"; }

  void stream(const AgentHistory &history, const ProviderOptions &,
              std::function<void(const StreamEvent &)> onEvent) override {
    callCount_.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      capturedUserTexts_.push_back(latestUserText(history));
    }
    onEvent(TextChunk{"ok"});
    onEvent(StreamDone{StopReason::Stop});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = "capture-user-history-model";
    model.provider = getId();
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }

  void generateSummary(const std::string &, const AgentHistory &history,
                       const std::string &,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> * = nullptr) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      capturedUserTexts_.push_back(latestUserText(history));
    }
    onEvent(TextChunk{"ok"});
    onEvent(StreamDone{StopReason::Stop});
  }

  ProviderType getProviderType() const override { return ProviderType::APIKey; }

  int callCount() const { return callCount_.load(); }

  std::vector<std::string> capturedUserTexts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capturedUserTexts_;
  }

private:
  std::atomic<int> callCount_{0};
  mutable std::mutex mutex_;
  std::vector<std::string> capturedUserTexts_;
};

class ReferencePersistenceHarnessTest : public ::testing::Test {
protected:
  void SetUp() override {
    originalHome_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    originalPromptsDir_ =
        std::getenv("FIRMIUS_PROMPTS_DIR") ? std::getenv("FIRMIUS_PROMPTS_DIR")
                                            : "";

    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_ref_persist_" +
                 std::to_string(static_cast<long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())));
    promptsDir_ = testHome_ / "prompts";
    std::filesystem::create_directories(testHome_ / ".firmius" / "threads");
    std::filesystem::create_directories(promptsDir_);
    setenv("HOME", testHome_.c_str(), 1);
    setenv("FIRMIUS_PROMPTS_DIR", promptsDir_.c_str(), 1);

    {
      std::ofstream base(promptsDir_ / "base.md");
      base << "Base prompt";
    }
    {
      std::ofstream lead(promptsDir_ / "lead.md");
      lead << "---\nname: lead\ntitle: Lead\nwork_role: lead\nscopes: [\"FilesystemRead\"]\n---\nLead";
    }

    Harness::instance().init();

    provider_ = std::make_shared<CaptureUserHistoryProvider>();
    ProviderRegistry::instance().registerProvider(provider_);
    auto cfg = ConfigLoader::instance().getConfig();
    cfg.defaultProviderId = provider_->getId();
    cfg.defaultModelId = provider_->listModels().front().id;
    cfg.defaultLeadPersona = "lead";
    Harness::instance().updateConfig(cfg);
  }

  void TearDown() override {
    Harness::instance().shutdown();
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

  std::shared_ptr<IAgent> waitForFocusedAgent() {
    std::shared_ptr<IAgent> agent;
    const bool found = waitForCondition([&]() {
      const auto id = Harness::instance().focusedAgentId();
      if (id.empty()) {
        return false;
      }
      agent = AgentRegistry::instance().getAgent(id);
      return agent && !agent->isBooting();
    });
    EXPECT_TRUE(found);
    return agent;
  }

  bool waitForIdle(const std::string &agentId) {
    return waitForCondition([&]() {
      auto agent = AgentRegistry::instance().getAgent(agentId);
      return agent && !agent->isRunning() &&
             agent->getContext().state.currentStatus == AgentStatus::Idle;
    });
  }

  std::filesystem::path testHome_;
  std::filesystem::path promptsDir_;
  std::string originalHome_;
  std::string originalPromptsDir_;
  std::shared_ptr<CaptureUserHistoryProvider> provider_;
};

TEST_F(ReferencePersistenceHarnessTest, PersistsExpandedXmlInsteadOfShorthand) {
  auto &harness = Harness::instance();
  const auto cwd = (testHome_ / "workspace");
  std::filesystem::create_directories(cwd);
  {
    std::ofstream file(cwd / "src.txt");
    file << "snapshot-content";
  }

  const std::string threadId = harness.newThread({}, cwd.string(), "lead");
  ASSERT_FALSE(threadId.empty());
  harness.send("inspect @src.txt");

  auto agent = waitForFocusedAgent();
  ASSERT_TRUE(agent);
  ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const auto history = tm.loadAgentHistory(threadId, agent->getContext().identity.id);

  bool foundExpandedUserTurn = false;
  for (const auto &turn : history.turns) {
    if (turn.turnId.rfind("user-task-", 0) != 0 || turn.messages.empty()) {
      continue;
    }
    const auto &msg = turn.messages.front();
    if (msg.role != Role::User) {
      continue;
    }
    for (const auto &part : msg.content) {
      const auto *text = std::get_if<TextContent>(&part);
      if (!text) {
        continue;
      }
      if (text->text.find("<file path=\"src.txt\">") != std::string::npos) {
        foundExpandedUserTurn = true;
        EXPECT_NE(text->text.find("snapshot-content"), std::string::npos);
        EXPECT_EQ(text->text.find("@src.txt"), std::string::npos);
      }
    }
  }
  EXPECT_TRUE(foundExpandedUserTurn);
}

TEST_F(ReferencePersistenceHarnessTest,
       RetryUsesPersistedExpandedContentWithoutRereadingFiles) {
  auto &harness = Harness::instance();
  const auto cwd = (testHome_ / "workspace");
  std::filesystem::create_directories(cwd);
  {
    std::ofstream file(cwd / "src.txt");
    file << "snapshot-v1";
  }

  const std::string threadId = harness.newThread({}, cwd.string(), "lead");
  ASSERT_FALSE(threadId.empty());
  harness.send("inspect @src.txt");
  auto agent = waitForFocusedAgent();
  ASSERT_TRUE(agent);
  ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));
  ASSERT_GE(provider_->callCount(), 1);

  std::filesystem::remove(cwd / "src.txt");

  std::string statusMessage;
  ASSERT_TRUE(harness.retryLastRequest(statusMessage));
  ASSERT_TRUE(waitForCondition([&]() { return provider_->callCount() >= 2; }));
  ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));

  const auto captured = provider_->capturedUserTexts();
  ASSERT_GE(captured.size(), 2u);
  EXPECT_NE(captured[0].find("<file path=\"src.txt\">"), std::string::npos);
  EXPECT_NE(captured[0].find("snapshot-v1"), std::string::npos);
  EXPECT_NE(captured[1].find("<file path=\"src.txt\">"), std::string::npos);
  EXPECT_NE(captured[1].find("snapshot-v1"), std::string::npos);
}

TEST_F(ReferencePersistenceHarnessTest,
       IncompleteArtifactPrefixPassesThroughAsLiteralUserText) {
  auto &harness = Harness::instance();
  const auto cwd = (testHome_ / "workspace");
  std::filesystem::create_directories(cwd);

  const std::string threadId = harness.newThread({}, cwd.string(), "lead");
  ASSERT_FALSE(threadId.empty());
  harness.send("inspect @artifact:");

  auto agent = waitForFocusedAgent();
  ASSERT_TRUE(agent);
  ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));
  EXPECT_EQ(provider_->callCount(), 1);

  const auto captured = provider_->capturedUserTexts();
  ASSERT_FALSE(captured.empty());
  EXPECT_EQ(captured.back(), "inspect @artifact:");
}

TEST_F(ReferencePersistenceHarnessTest,
       TrailingArtifactPunctuationStillExpandsBeforeDispatch) {
  auto &harness = Harness::instance();
  const auto cwd = (testHome_ / "workspace");
  std::filesystem::create_directories(cwd);

  const std::string threadId = harness.newThread({}, cwd.string(), "lead");
  ASSERT_FALSE(threadId.empty());

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  tm.writeArtifact(threadId, "lead-agent", "lead", "REPORT.md", "artifact-body");

  harness.send("review (@artifact:REPORT.md).");
  auto agent = waitForFocusedAgent();
  ASSERT_TRUE(agent);
  ASSERT_TRUE(waitForIdle(agent->getContext().identity.id));

  const auto captured = provider_->capturedUserTexts();
  ASSERT_FALSE(captured.empty());
  EXPECT_NE(captured.back().find("<artifact path=\"lead/REPORT.md\">"),
            std::string::npos);
  EXPECT_TRUE(!captured.back().empty() && captured.back().back() == '.');
}

} // namespace
