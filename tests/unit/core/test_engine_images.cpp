#include <gtest/gtest.h>

#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "persistence/ThreadManager.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

template <typename Fn>
bool waitForEngineCondition(
    Fn&& fn, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
    std::chrono::milliseconds step = std::chrono::milliseconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) {
      return true;
    }
    std::this_thread::sleep_for(step);
  }
  return fn();
}

class EngineImagesTest : public ::testing::Test {
protected:
  void SetUp() override {
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_engine_test_" + std::to_string(getpid()));
    std::filesystem::create_directories(testHome_);
    setenv("HOME", testHome_.c_str(), 1);
    std::filesystem::create_directories(testHome_ / ".firmius" / "threads");

    auto sourcePath = std::filesystem::path(__FILE__);
    auto repoRoot = sourcePath.parent_path().parent_path().parent_path().parent_path();
    auto promptsDir = repoRoot / "prompts";
    setenv("FIRMIUS_PROMPTS_DIR", promptsDir.c_str(), 1);
  }

  void TearDown() override {
    for (const auto &agentId : AgentRegistry::instance().listAll()) {
      Engine::instance().terminateAgent(agentId);
    }
    unsetenv("FIRMIUS_PROMPTS_DIR");
    std::filesystem::remove_all(testHome_);
  }

  std::filesystem::path testHome_;
};

TEST_F(EngineImagesTest, SummonAgentPreservesInitialImagesInUserTaskTurn) {
  ThreadMetadata metadata;
  metadata.title = "Retry Images";
  metadata.hostOptions.type = HostType::Local;
  metadata.hostIdentifier = "localhost";
  metadata.cwd = "/tmp";
  metadata.leadPersona = "lead";

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const std::string threadId = tm.createThread(metadata);

  const std::vector<ImageContent> images = {
      {"data:image/png;base64,abc123", "image/png", "auto"}};
  const std::string agentId =
      Engine::instance().summonAgent(threadId, "lead", "Retry with image",
                                     true, "", "lead", "", "", "", "", "",
                                     images);

  AgentHistory history;
  ASSERT_TRUE(waitForEngineCondition([&]() {
    if (auto agent = AgentRegistry::instance().getAgent(agentId)) {
      history = *agent->getContext().history;
    } else {
      history = tm.loadAgentHistory(threadId, agentId);
    }

    for (const auto &turn : history.turns) {
      if (turn.turnId.rfind("user-task-", 0) != 0 || turn.messages.empty()) {
        continue;
      }
      for (const auto &part : turn.messages.front().content) {
        if (std::holds_alternative<ImageContent>(part)) {
          return true;
        }
      }
    }
    return false;
  })) << "Timed out waiting for initial user-task image content";

  bool foundImage = false;
  for (const auto &turn : history.turns) {
    if (turn.turnId.rfind("user-task-", 0) != 0 || turn.messages.empty()) {
      continue;
    }
    for (const auto &part : turn.messages.front().content) {
      if (auto *image = std::get_if<ImageContent>(&part)) {
        foundImage = true;
        EXPECT_EQ(image->url, "data:image/png;base64,abc123");
        EXPECT_EQ(image->mediaType, "image/png");
        EXPECT_EQ(image->detail, "auto");
      }
    }
  }
  EXPECT_TRUE(foundImage);
}

} // namespace
