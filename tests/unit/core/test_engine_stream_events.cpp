#include <gtest/gtest.h>
#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "persistence/ThreadManager.hpp"
#include "Events.hpp"
#include "agents/Agent.hpp"
#include "IEnvironment.hpp"
#include <chrono>
#include <filesystem>
#include <thread>
#include <variant>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

template <typename Fn>
bool waitForCondition(Fn &&fn,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return fn();
}

class EngineStreamEventsTest : public ::testing::Test {
protected:
  void SetUp() override {
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_engine_test_stream_" + 
                 std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
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

TEST_F(EngineStreamEventsTest, AgentProcessOutputIsForwardedFromEnvironment) {
  ThreadMetadata metadata;
  metadata.title = "Stream Test";
  metadata.hostOptions.type = HostType::Local;
  metadata.hostIdentifier = "localhost";
  metadata.cwd = "/tmp";
  metadata.leadPersona = "lead";

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  const std::string threadId = tm.createThread(metadata);

  // Summon agent - this uses spawnAgent internally which has the no-op callback
  const std::string agentId =
      Engine::instance().summonAgent(threadId, "lead", "dummy task",
                                     true, "", "lead", "", "", "", "", "");

  // Wait for agent to be registered
  ASSERT_TRUE(waitForCondition([&]() {
    return AgentRegistry::instance().getAgent(agentId) != nullptr;
  })) << "Agent not summoned in time";

  auto agent = AgentRegistry::instance().getAgent(agentId);
  ASSERT_NE(agent, nullptr);

  // Catch broadcasts
  bool outputReceived = false;
  std::string receivedOutput;
  Engine::instance().addEventListener([&](const AppEvent &event) {
    if (auto *out = std::get_if<AgentProcessOutput>(&event)) {
      if (out->agentId == agentId) {
        outputReceived = true;
        receivedOutput += out->output;
      }
    }
  });

  // Induce process output directly from environment.
  // The Environment was created with the no-op callback in Engine::spawnAgent.
  // If the fix is NOT applied, this output will be swallowed.
  agent->getEnvironment()->getProcessManager().spawnProcess("echo hello_stream", "tool-1", "/tmp", {}, false);

  // Wait for the output event
  ASSERT_TRUE(waitForCondition([&]() {
    return outputReceived;
  }, std::chrono::milliseconds(5000))) << "Timed out waiting for AgentProcessOutput (callback likely no-op)";

  EXPECT_TRUE(receivedOutput.find("hello_stream") != std::string::npos);
}

} // namespace
