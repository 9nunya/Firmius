#include "AgentRegistry.hpp"
#include "IAgent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "agents/PurposeLoader.hpp"
#include "tools/SubagentTool.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace firmius::core;
using namespace firmius::shared;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

class MockHost : public IHost {
public:
  MOCK_METHOD(std::string, init, (), (override));
  MOCK_METHOD(void, destroy, (), (override));
  MOCK_METHOD(void, cleanup, (), (override));
  MOCK_METHOD(void, setUser, (const std::string &), (override));
  MOCK_METHOD(std::vector<uint8_t>, readFile, (const std::string &),
              (override));
  MOCK_METHOD(void, writeFile,
              (const std::string &, (const std::vector<uint8_t> &)),
              (override));
  MOCK_METHOD(bool, exists, (const std::string &), (override));
  MOCK_METHOD(std::vector<FileInfo>, listDir, (const std::string &),
              (override));
  MOCK_METHOD(FileInfo, stat, (const std::string &), (override));
  MOCK_METHOD(std::string, getId, (), (const, override));
  MOCK_METHOD((ProcessResult), exec,
              (const std::string &, const std::string &,
               (const std::map<std::string, std::string> &),
               std::optional<std::chrono::milliseconds>),
              (override));
  MOCK_METHOD((std::unique_ptr<IHostProcess>), spawn,
              (const std::string &, const std::string &,
               (const std::map<std::string, std::string> &)),
              (override));
  MOCK_METHOD(void, registerBackgroundProcess,
              (const std::string &, (std::unique_ptr<IHostProcess>)),
              (override));
  MOCK_METHOD(ProcessSnapshot, inspectBackgroundProcess, (const std::string &),
              (override));
  MOCK_METHOD(void, writeToBackgroundProcess,
              (const std::string &, const std::string &), (override));
  MOCK_METHOD(void, killBackgroundProcess, (const std::string &), (override));
};

class MockAgent : public IAgent {
public:
  firmius::shared::AgentContext defaultCtx;
  std::unique_ptr<AgentPermissionChecks> pChecks;
  MockAgent() { pChecks = std::make_unique<AgentPermissionChecks>(defaultCtx); }
  AgentPermissionChecks &getPermissionChecks() const override {
    return *pChecks;
  }
  MOCK_METHOD(void, reset, (), (override));
  MOCK_METHOD(void, run,
              (const std::string &, (std::function<void(const StreamEvent &)>)),
              (override));
  MOCK_METHOD((const AgentContext &), getContext, (), (const, override));
  MOCK_METHOD(AgentContext &, getMutableContext, (), (override));
  MOCK_METHOD(std::string, resolvePath, (const std::string &),
              (const, override));
  MOCK_METHOD(void, interrupt, (), (override));
  MOCK_METHOD(bool, isInterrupted, (), (const, override));
  MOCK_METHOD(void, setModel, (const std::string &, const std::string &),
              (override));
  MOCK_METHOD(void, setModel,
              (const std::string &, const std::string &, const std::string &),
              (override));
  MOCK_METHOD(bool, isRunning, (), (const, override));
  MOCK_METHOD(bool, isBooting, (), (const, override));
  MOCK_METHOD(void, setBooting, (bool), (override));
  MOCK_METHOD(void, emitProcessSpawned,
              (const std::string &, const std::string &, const std::string &),
              (override));
  MOCK_METHOD(std::string, spawnProcess,
              (const std::string &, const std::string &, const std::string &,
               (const std::map<std::string, std::string> &)),
              (override));
  MOCK_METHOD(ProcessSnapshot, inspectProcess, (const std::string &),
              (override));
  MOCK_METHOD(void, writeToProcess, (const std::string &, const std::string &),
              (override));
  MOCK_METHOD(void, registerProcessId, (const std::string &), (override));
  MOCK_METHOD(void, addBlockingProcessId, (const std::string &), (override));
  MOCK_METHOD(void, removeBlockingProcessId, (const std::string &), (override));
  MOCK_METHOD((std::vector<std::string>), getBlockingProcessIds, (),
              (override));
  MOCK_METHOD(bool, hasReadFile, (const std::string &), (const, override));
  MOCK_METHOD(void, markFileAsRead, (const std::string &), (override));
  MOCK_METHOD((std::shared_ptr<IHost>), getHost, (), (override));
};

class SubagentToolTest : public ::testing::Test {
protected:
  void SetUp() override {
    testPromptsDir = std::filesystem::temp_directory_path() /
                     "firmius_subagent_test_prompts";
    std::filesystem::create_directories(testPromptsDir);
    setenv("FIRMIUS_PROMPTS_DIR", testPromptsDir.c_str(), 1);

    // Create a valid persona file
    std::ofstream coderFile(testPromptsDir / "coder.md");
    coderFile << "---\nname: coder\ntitle: Coder\n---\nCoder identity";
    coderFile.close();
  }

  void TearDown() override {
    std::filesystem::remove_all(testPromptsDir);
    unsetenv("FIRMIUS_PROMPTS_DIR");
  }

  std::filesystem::path testPromptsDir;
};

TEST_F(SubagentToolTest, validation_fails_for_invalid_persona) {
  SubagentTool tool;
  SubagentInput input;
  input.persona = "invalid_persona";
  input.task = "some task";
  input.name = "subagent-1";
  input.title = "Subagent 1";

  MockAgent agent;
  AgentContext ctx_obj;
  ctx_obj.history = std::make_shared<AgentHistory>();
  ctx_obj.history->threadId = "test-thread";

  EXPECT_CALL(agent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, agent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, ::testing::HasSubstr("Invalid persona"));
}

TEST_F(SubagentToolTest, validation_passes_for_valid_persona) {
  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "some task";
  input.name = "subagent-1";
  input.title = "Subagent 1";

  MockAgent agent;
  AgentContext ctx_obj;
  ctx_obj.history = std::make_shared<AgentHistory>();
  ctx_obj.history->threadId = "test-thread";
  ctx_obj.identity.id = "parent-id";
  ctx_obj.identity.friendlyName = "parent";

  EXPECT_CALL(agent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, agent, "test-call-id"};

  // We expect validation to pass, but the engine call to fail because the
  // environment isn't set up. However, we want to ensure it doesn't fail with
  // the "Invalid persona" error.
  ToolResult result = tool.execute(input, toolCtx);
  if (!result.success) {
    EXPECT_THAT(result.error,
                ::testing::Not(::testing::HasSubstr("Invalid persona")));
  }
}

TEST_F(SubagentToolTest, concurrent_validation_requests) {
  const int numThreads = 5;
  const int requestsPerThread = 10;
  std::atomic<int> successCount{0};
  std::atomic<int> failureCount{0};

  auto runTest = [&]() {
    SubagentTool tool;
    MockAgent agent;
    AgentContext ctx_obj;
    ctx_obj.history = std::make_shared<AgentHistory>();
    ctx_obj.history->threadId = "test-thread";
    EXPECT_CALL(agent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));
    NiceMock<MockHost> host;
    ToolContext toolCtx{host, agent, "test-call-id"};

    for (int i = 0; i < requestsPerThread; ++i) {
      SubagentInput input;
      bool shouldSucceed = (i % 2 == 0);
      input.persona = shouldSucceed ? "coder" : "invalid";
      input.task = "task";
      input.name = "sub-" + std::to_string(i);
      input.title = "title";

      // Only test the validation logic in concurrent test to avoid Engine
      // interference
      if (PurposeLoader::isValid(input.persona) == shouldSucceed) {
        successCount++;
      } else {
        failureCount++;
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < numThreads; ++i) {
    threads.emplace_back(runTest);
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(failureCount, 0);
  EXPECT_EQ(successCount, numThreads * requestsPerThread);
}
