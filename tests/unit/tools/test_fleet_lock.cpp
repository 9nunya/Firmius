#include "tools/FleetLockTool.hpp"
#include "tools/FleetLockRespondTool.hpp"
#include "tools/FleetStatusTool.hpp"
#include "persistence/ThreadManager.hpp"
#include "harness/Harness.hpp"
#include "hosts/LocalHost.hpp"
#include "../mocks/MockEnvironment.hpp"
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <chrono>
#include <cstdlib>

using namespace firmius::core;
using namespace firmius::shared;
using ::testing::_;
using ::testing::Return;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::IsEmpty;

namespace {

void cleanupTestThread(const std::filesystem::path &threadsBase,
                       const std::string &threadId) {
  std::filesystem::remove_all(threadsBase / threadId);
}

std::string createTestThread(const std::filesystem::path &threadsBase) {
  std::string threadId =
      "test-thread-" +
      std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(threadsBase / threadId);
  return threadId;
}

rapidjson::Document parseJson(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  return doc;
}

class TestAgent : public IAgent {
public:
  TestAgent(const std::string &threadId, const std::string &agentId) {
    context_.history = std::make_shared<AgentHistory>();
    context_.history->threadId = threadId;
    context_.identity.id = agentId;
    context_.identity.parentId = "";
    environment_ = std::make_shared<firmius::test::MockEnvironment>();
    permissions_ = std::make_shared<firmius::test::MockPermissions>();
  }

  void reset() override {}
  void run(const std::string &, std::function<void(const StreamEvent &)>,
           const std::vector<ImageContent> &) override {}
  void resume(std::function<void(const StreamEvent &)>) override {}
  void interrupt() override {}
  bool isInterrupted() const override { return false; }
  void clearInterrupt() override {}
  void compactNow(std::function<void(const StreamEvent &)>) override {}
  void setModel(const std::string &, const std::string &) override {}
  void setModel(const std::string &, const std::string &,
                const std::string &) override {}
  bool isRunning() const override { return false; }
  bool isBooting() const override { return false; }
  void setBooting(bool) override {}
  const AgentContext &getContext() const override { return context_; }
  AgentContext &getMutableContext() override { return context_; }
  void saveHistory() override {}
  void appendHistoryTurn(const AgentTurn &turn) override {
    if (!context_.history) {
      context_.history = std::make_shared<AgentHistory>();
    }
    context_.history->turns.push_back(turn);
  }

  ModelChoice getPreferredModel() const override {
    ModelChoice choice;
    choice.providerId = context_.config.providerId;
    choice.modelId = context_.config.modelId;
    return choice;
  }

  std::shared_ptr<IEnvironment> getEnvironment() const override {
    return environment_;
  }
  std::shared_ptr<IPermissions> getPermissions() const override {
    return permissions_;
  }
  std::shared_ptr<firmius::shared::IHost> getHost() override {
    return environment_->getHost();
  }

private:
  AgentContext context_;
  std::shared_ptr<IEnvironment> environment_;
  std::shared_ptr<IPermissions> permissions_;
};

} // namespace

class FleetLockToolTest : public ::testing::Test {
protected:
  std::string testThreadId_;
  std::string testAgentId_;
  std::unique_ptr<ThreadManager> tm_;
  std::shared_ptr<LocalHost> host_;
  std::shared_ptr<TestAgent> agent_;
  std::filesystem::path testHome_;
  std::filesystem::path threadsBase_;
  std::string originalHome_;
  bool hadHome_ = false;

  void SetUp() override {
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_fleet_lock_home_" +
                 std::to_string(std::chrono::system_clock::now()
                                    .time_since_epoch()
                                    .count()));
    threadsBase_ = testHome_ / ".firmius" / "threads";
    std::filesystem::create_directories(threadsBase_);
    hadHome_ = std::getenv("HOME") != nullptr;
    originalHome_ = hadHome_ ? std::getenv("HOME") : "";
    setenv("HOME", testHome_.c_str(), 1);

    testThreadId_ = createTestThread(threadsBase_);
    testAgentId_ = "test-agent-" +
                   std::to_string(std::chrono::system_clock::now()
                                      .time_since_epoch()
                                      .count());
    tm_ = std::make_unique<ThreadManager>(ThreadManager::defaultBasePath());
    host_ = std::make_shared<LocalHost>();
    agent_ = std::make_shared<TestAgent>(testThreadId_, testAgentId_);
  }

  void TearDown() override {
    cleanupTestThread(threadsBase_, testThreadId_);
    std::filesystem::remove_all(testHome_);
    if (hadHome_) {
      setenv("HOME", originalHome_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
  }

  ToolContext makeToolContext() {
    return ToolContext{*host_, *agent_, "test-call"};
  }
};

TEST_F(FleetLockToolTest, AcquireModeRequiresReason) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  FleetLockInput input;
  input.mode = "acquire";
  input.paths = {"test.cpp"};

  auto result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("reason"));
}

TEST_F(FleetLockToolTest, AcquireModeRequiresPaths) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  FleetLockInput input;
  input.mode = "acquire";
  input.reason = "Testing";

  auto result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("paths"));
}

TEST_F(FleetLockToolTest, AcquireModeCreatesLock) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  FleetLockInput input;
  input.mode = "acquire";
  input.reason = "Testing lock acquisition";
  input.paths = {"src/test.cpp", "include/test.hpp"};

  auto result = tool.execute(input, ctx);
  EXPECT_TRUE(result.success);

  rapidjson::Document resultDoc = parseJson(result.data);
  EXPECT_TRUE(resultDoc.HasMember("lock_id"));
  std::string lockId = resultDoc["lock_id"].GetString();
  EXPECT_THAT(lockId, Not(IsEmpty()));

  // Verify lock was persisted
  FleetState state = tm_->getFleetState(testThreadId_);
  EXPECT_EQ(state.locks.size(), 1);
  EXPECT_EQ(state.locks[0].lockId, lockId);
  EXPECT_EQ(state.locks[0].reason, "Testing lock acquisition");
  EXPECT_EQ(state.locks[0].paths.size(), 2);
}

TEST_F(FleetLockToolTest, ReleaseModeRequiresLockId) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  FleetLockInput input;
  input.mode = "release";

  auto result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("lock_id"));
}

TEST_F(FleetLockToolTest, ReleaseModeFailsForNonExistentLock) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  FleetLockInput input;
  input.mode = "release";
  input.lock_id = "nonexistent-lock";

  auto result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("not found"));
}

TEST_F(FleetLockToolTest, ReleaseModeSuccessfullyReleasesLock) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  // First acquire a lock
  FleetLockInput acquireInput;
  acquireInput.mode = "acquire";
  acquireInput.reason = "Test";
  acquireInput.paths = {"test.cpp"};

  auto acquireResult = tool.execute(acquireInput, ctx);
  EXPECT_TRUE(acquireResult.success);

  rapidjson::Document resultDoc = parseJson(acquireResult.data);
  std::string lockId = resultDoc["lock_id"].GetString();

  // Release the lock
  FleetLockInput releaseInput;
  releaseInput.mode = "release";
  releaseInput.lock_id = lockId;

  auto releaseResult = tool.execute(releaseInput, ctx);
  EXPECT_TRUE(releaseResult.success);

  // Verify lock was released
  FleetState state = tm_->getFleetState(testThreadId_);
  EXPECT_EQ(state.locks[0].status, "released");
}

TEST_F(FleetLockToolTest, CheckModeReturnsLockStatus) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  // Create a lock
  FleetLockInput acquireInput;
  acquireInput.mode = "acquire";
  acquireInput.reason = "Test";
  acquireInput.paths = {"src/test.cpp"};

  auto acquireResult = tool.execute(acquireInput, ctx);
  EXPECT_TRUE(acquireResult.success);

  // Check lock status
  FleetLockInput checkInput;
  checkInput.mode = "check";
  checkInput.paths = {"src/test.cpp"};

  auto checkResult = tool.execute(checkInput, ctx);
  EXPECT_TRUE(checkResult.success);

  rapidjson::Document resultDoc = parseJson(checkResult.data);
  EXPECT_TRUE(resultDoc.HasMember("has_conflicts"));
  EXPECT_TRUE(resultDoc["has_conflicts"].GetBool());
  EXPECT_TRUE(resultDoc.HasMember("locks"));
  EXPECT_GT(resultDoc["locks"].GetArray().Size(), 0);
}

TEST_F(FleetLockToolTest, CheckModeNoConflicts) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  FleetLockInput checkInput;
  checkInput.mode = "check";
  checkInput.paths = {"src/nonexistent.cpp"};

  auto checkResult = tool.execute(checkInput, ctx);
  EXPECT_TRUE(checkResult.success);

  rapidjson::Document resultDoc = parseJson(checkResult.data);
  EXPECT_TRUE(resultDoc.HasMember("has_conflicts"));
  EXPECT_FALSE(resultDoc["has_conflicts"].GetBool());
}

TEST_F(FleetLockToolTest, WaitModeTimesOut) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  // Create a lock
  FleetLockInput acquireInput;
  acquireInput.mode = "acquire";
  acquireInput.reason = "Test";
  acquireInput.paths = {"test.cpp"};

  auto acquireResult = tool.execute(acquireInput, ctx);
  rapidjson::Document resultDoc = parseJson(acquireResult.data);
  std::string lockId = resultDoc["lock_id"].GetString();

  // Wait with short timeout (lock won't be released)
  FleetLockInput waitInput;
  waitInput.mode = "wait";
  waitInput.lock_id = lockId;
  waitInput.timeout_ms = 100;

  auto startTime = std::chrono::steady_clock::now();
  auto waitResult = tool.execute(waitInput, ctx);
  auto endTime = std::chrono::steady_clock::now();

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

  EXPECT_FALSE(waitResult.success);
  EXPECT_THAT(waitResult.error, HasSubstr("timed out"));
  EXPECT_GE(duration.count(), 100);
}

TEST_F(FleetLockToolTest, RequestModeRequiresTargetAgentId) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  FleetLockInput input;
  input.mode = "request";
  input.paths = {"test.cpp"};
  input.reason = "Test request";

  auto result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("target_agent_id"));
}

TEST_F(FleetLockToolTest, RequestModeRequiresPaths) {
  FleetLockTool tool;
  ToolContext ctx = makeToolContext();

  FleetLockInput input;
  input.mode = "request";
  input.target_agent_id = "agent-b";

  auto result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("paths"));
}

TEST_F(FleetLockToolTest, FleetStatusToolReturnsAllLocks) {
  FleetLockTool lockTool;
  FleetStatusTool statusTool;
  ToolContext ctx = makeToolContext();

  // Create multiple locks
  for (int i = 0; i < 3; ++i) {
    FleetLockInput acquireInput;
    acquireInput.mode = "acquire";
    acquireInput.reason = "Test lock " + std::to_string(i);
    acquireInput.paths = {"test" + std::to_string(i) + ".cpp"};
    lockTool.execute(acquireInput, ctx);
  }

  // Get status
  FleetStatusInput statusInput;
  auto statusResult = statusTool.execute(statusInput, ctx);
  EXPECT_TRUE(statusResult.success);

  rapidjson::Document resultDoc = parseJson(statusResult.data);
  EXPECT_TRUE(resultDoc.HasMember("locks"));
  EXPECT_EQ(resultDoc["locks"].GetArray().Size(), 3);
}

TEST_F(FleetLockToolTest, FleetStatusToolFiltersByRootAgent) {
  FleetLockTool lockTool;
  FleetStatusTool statusTool;
  ToolContext ctx = makeToolContext();

  // Create a lock
  FleetLockInput acquireInput;
  acquireInput.mode = "acquire";
  acquireInput.reason = "Test";
  acquireInput.paths = {"test.cpp"};
  lockTool.execute(acquireInput, ctx);

  // Get status with non-existent root agent
  FleetStatusInput statusInput;
  statusInput.root_agent_id = "non-existent-agent";
  auto statusResult = statusTool.execute(statusInput, ctx);
  EXPECT_TRUE(statusResult.success);

  rapidjson::Document resultDoc = parseJson(statusResult.data);
  EXPECT_EQ(resultDoc["locks"].GetArray().Size(), 0);
}

TEST_F(FleetLockToolTest, FleetStatusToolIncludesClosedWhenRequested) {
  FleetLockTool lockTool;
  FleetStatusTool statusTool;
  ToolContext ctx = makeToolContext();

  // Create and release a lock
  FleetLockInput acquireInput;
  acquireInput.mode = "acquire";
  acquireInput.reason = "Test";
  acquireInput.paths = {"test.cpp"};
  auto acquireResult = lockTool.execute(acquireInput, ctx);

  rapidjson::Document resultDoc = parseJson(acquireResult.data);
  std::string lockId = resultDoc["lock_id"].GetString();

  FleetLockInput releaseInput;
  releaseInput.mode = "release";
  releaseInput.lock_id = lockId;
  lockTool.execute(releaseInput, ctx);

  // Get status without include_closed
  FleetStatusInput statusInput1;
  statusInput1.include_closed = false;
  auto statusResult1 = statusTool.execute(statusInput1, ctx);
  rapidjson::Document resultDoc1 = parseJson(statusResult1.data);
  EXPECT_EQ(resultDoc1["locks"].GetArray().Size(), 0);

  // Get status with include_closed
  FleetStatusInput statusInput2;
  statusInput2.include_closed = true;
  auto statusResult2 = statusTool.execute(statusInput2, ctx);
  rapidjson::Document resultDoc2 = parseJson(statusResult2.data);
  EXPECT_EQ(resultDoc2["locks"].GetArray().Size(), 1);
}

// FleetLockRespondTool tests
class FleetLockRespondToolTest : public ::testing::Test {
protected:
  std::string testThreadId_;
  std::string testAgentId_;
  std::shared_ptr<TestAgent> agent_;
  std::shared_ptr<LocalHost> host_;
  std::filesystem::path testHome_;
  std::filesystem::path threadsBase_;
  std::string originalHome_;
  bool hadHome_ = false;

  void SetUp() override {
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_fleet_lock_respond_home_" +
                 std::to_string(std::chrono::system_clock::now()
                                    .time_since_epoch()
                                    .count()));
    threadsBase_ = testHome_ / ".firmius" / "threads";
    std::filesystem::create_directories(threadsBase_);
    hadHome_ = std::getenv("HOME") != nullptr;
    originalHome_ = hadHome_ ? std::getenv("HOME") : "";
    setenv("HOME", testHome_.c_str(), 1);

    testThreadId_ = createTestThread(threadsBase_);
    testAgentId_ = "test-agent-" +
                   std::to_string(std::chrono::system_clock::now()
                                      .time_since_epoch()
                                      .count());
    host_ = std::make_shared<LocalHost>();
    agent_ = std::make_shared<TestAgent>(testThreadId_, testAgentId_);
  }

  void TearDown() override {
    cleanupTestThread(threadsBase_, testThreadId_);
    std::filesystem::remove_all(testHome_);
    if (hadHome_) {
      setenv("HOME", originalHome_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
  }

  ToolContext makeToolContext() {
    return ToolContext{*host_, *agent_, "test-call"};
  }
};

TEST_F(FleetLockRespondToolTest, AcceptCreatesLock) {
  FleetLockRespondTool tool;
  ToolContext ctx = makeToolContext();

  FleetLockRespondInput input;
  input.request_id = "test-request-123";
  input.accept = true;
  input.estimated_ms = 30000;

  auto result = tool.execute(input, ctx);
  EXPECT_TRUE(result.success);

  rapidjson::Document resultDoc = parseJson(result.data);
  EXPECT_TRUE(resultDoc.HasMember("accepted"));
  EXPECT_TRUE(resultDoc["accepted"].GetBool());
  EXPECT_TRUE(resultDoc.HasMember("lock_id"));
  EXPECT_THAT(resultDoc["lock_id"].GetString(), HasSubstr("req-test-request-123"));
}

TEST_F(FleetLockRespondToolTest, DenyReturnsDenial) {
  FleetLockRespondTool tool;
  ToolContext ctx = makeToolContext();

  FleetLockRespondInput input;
  input.request_id = "test-request-456";
  input.accept = false;
  input.deny_reason = "Already completed";

  auto result = tool.execute(input, ctx);
  EXPECT_TRUE(result.success);

  rapidjson::Document resultDoc = parseJson(result.data);
  EXPECT_TRUE(resultDoc.HasMember("accepted"));
  EXPECT_FALSE(resultDoc["accepted"].GetBool());
  EXPECT_TRUE(resultDoc.HasMember("deny_reason"));
  EXPECT_STREQ(resultDoc["deny_reason"].GetString(), "Already completed");
}
