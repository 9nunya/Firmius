#include "AgentRegistry.hpp"
#include "IAgent.hpp"
#include "Engine.hpp"
#include "ConfigLoader.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "agents/PurposeLoader.hpp"
#include "persistence/ThreadManager.hpp"
#include "tools/SubagentTool.hpp"
#include "Context.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <optional>
#include <thread>
#include <vector>
#include <set>

using namespace firmius::core;
using namespace firmius::shared;
using ::testing::_;
using ::testing::Invoke;
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

#include "../mocks/MockEnvironment.hpp"

// Local MockAgent with MOCK_METHOD for testing
class MockAgent : public IAgent {
public:
  firmius::shared::AgentContext defaultCtx;
  std::shared_ptr<firmius::test::MockEnvironment> mockEnv_;
  std::shared_ptr<firmius::test::MockPermissions> mockPerms_;

  MockAgent()
    : mockEnv_(std::make_shared<firmius::test::MockEnvironment>())
    , mockPerms_(std::make_shared<firmius::test::MockPermissions>()) {
    if (!defaultCtx.history) {
      defaultCtx.history = std::make_shared<AgentHistory>();
    }
  }

  ~MockAgent() override = default;

  std::shared_ptr<IEnvironment> getEnvironment() const override { return mockEnv_; }
  std::shared_ptr<IPermissions> getPermissions() const override { return mockPerms_; }

  MOCK_METHOD(void, reset, (), (override));
  MOCK_METHOD(void, run,
              (const std::string &, (std::function<void(const StreamEvent &)>),
               const std::vector<ImageContent> &),
              (override));
  MOCK_METHOD(void, resume, ((std::function<void(const StreamEvent &)>)),
              (override));
  MOCK_METHOD((const AgentContext &), getContext, (), (const, override));
  MOCK_METHOD(AgentContext &, getMutableContext, (), (override));
  MOCK_METHOD(void, interrupt, (), (override));
  MOCK_METHOD(bool, isInterrupted, (), (const, override));
  MOCK_METHOD(void, clearInterrupt, (), (override));
  MOCK_METHOD(void, compactNow,
              (std::function<void(const StreamEvent &)>), (override));
  MOCK_METHOD(void, saveHistory, (), (override));
  MOCK_METHOD(void, setModel, (const std::string &, const std::string &),
              (override));
  MOCK_METHOD(void, setModel,
              (const std::string &, const std::string &, const std::string &),
              (override));
  MOCK_METHOD(bool, isRunning, (), (const, override));
  MOCK_METHOD(bool, isBooting, (), (const, override));
  MOCK_METHOD(void, setBooting, (bool), (override));
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

    std::ofstream executorFile(testPromptsDir / "executor.md");
    executorFile << "---\nname: executor\ntitle: Executor\n---\nExecutor identity";
    executorFile.close();

    std::ofstream workerFile(testPromptsDir / "worker.md");
    workerFile << "---\nname: worker\ntitle: Worker\n---\nWorker identity";
    workerFile.close();

    auto unique =
        "firmius_subagent_home_" +
        std::to_string(static_cast<long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    testHome_ = std::filesystem::temp_directory_path() / unique;
    std::filesystem::create_directories(testHome_ / ".firmius" / "threads");

    const char *existingHome = std::getenv("HOME");
    if (existingHome) {
      originalHome_ = existingHome;
    }
    setenv("HOME", testHome_.c_str(), 1);

    firmius::shared::ConfigLoader::instance().updateConfig(
        firmius::shared::UserConfig{});

    threadManager_ =
        std::make_unique<ThreadManager>((testHome_ / ".firmius" / "threads").string());
  }

  void TearDown() override {
    Engine::instance().shutdown();
    for (const auto &agentId : registeredAgentIds_) {
      AgentRegistry::instance().unregisterAgent(agentId);
    }
    registeredAgents_.clear();
    std::filesystem::remove_all(testPromptsDir);
    std::filesystem::remove_all(testHome_);
    unsetenv("FIRMIUS_PROMPTS_DIR");
    if (originalHome_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", originalHome_.c_str(), 1);
    }
  }

  std::string createThread(const std::string &cwd = "/tmp/project") {
    ThreadMetadata metadata;
    metadata.title = "Subagent Test";
    metadata.cwd = cwd;
    metadata.hostOptions.type = HostType::Local;
    metadata.leadPersona = "lead";
    return threadManager_->createThread(metadata);
  }

  std::string createPlanWithChunks(const std::string &threadId) {
    Plan plan;
    plan.threadId = threadId;
    plan.id = "plan-1";
    plan.title = "Migrate work language";
    plan.objective = "Ship chunk-aware delegation";
    plan.strategy = "Lead delegates by persisted chunk state";

    WorkChunk primary;
    primary.id = "chunk-1";
    primary.title = "Integrate delegation ownership";
    primary.goal = "Make executor delegation chunk-aware";
    primary.context = "The lead should wake executors from persisted chunk data";
    primary.constraints = "Do not expose sibling chunks";
    primary.completion = "Executor owns one chunk with bounded worker helpers";
    primary.status = WorkChunkStatus::Ready;

    WorkChunk sibling;
    sibling.id = "chunk-2";
    sibling.title = "Sibling chunk should stay hidden";
    sibling.goal = "This should never appear in executor context";
    sibling.context = "Unrelated";
    sibling.constraints = "Keep hidden";
    sibling.completion = "Hidden";
    sibling.status = WorkChunkStatus::Ready;

    plan.chunks = {primary, sibling};
    threadManager_->writePlan(threadId, plan);
    return plan.id;
  }

  std::string createPlanWithDependency(const std::string &threadId,
                                       WorkChunkStatus dependencyStatus,
                                       WorkChunkStatus targetStatus =
                                           WorkChunkStatus::Ready) {
    Plan plan;
    plan.threadId = threadId;
    plan.id = "plan-deps";
    plan.title = "Dependency-gated plan";
    plan.objective = "Ensure dispatch waits for done dependencies";
    plan.strategy = "Lead dispatches only executable chunks";

    WorkChunk dependency;
    dependency.id = "dep-1";
    dependency.title = "Dependency chunk";
    dependency.goal = "Finish prerequisite work";
    dependency.context = "Upstream dependency";
    dependency.constraints = "Must finish first";
    dependency.completion = "Dependency complete";
    dependency.status = dependencyStatus;

    WorkChunk target;
    target.id = "chunk-1";
    target.title = "Blocked until dependency done";
    target.goal = "Dispatch only when prerequisite is done";
    target.context = "Depends on dep-1";
    target.constraints = "Do not dispatch early";
    target.completion = "Executor starts only after dep-1 is done";
    target.status = targetStatus;
    target.dependsOn = {"dep-1"};

    plan.chunks = {dependency, target};
    threadManager_->writePlan(threadId, plan);
    return plan.id;
  }

  AgentContext makeParentContext(const std::string &threadId,
                                 const std::string &persona = "lead") {
    AgentContext ctx;
    ctx.history = std::make_shared<AgentHistory>();
    ctx.history->threadId = threadId;
    ctx.identity.id = "parent-agent";
    ctx.identity.friendlyName = "parent";
    ctx.config.personaName = persona;
    return ctx;
  }

  std::shared_ptr<NiceMock<MockAgent>>
  registerRetaskableAgent(const std::string &agentId,
                          const std::string &friendlyName,
                          std::shared_ptr<std::promise<std::string>> taskPromise) {
    auto agent = std::make_shared<NiceMock<MockAgent>>();
    auto *agentPtr = agent.get();
    agent->defaultCtx.history = std::make_shared<AgentHistory>();
    agent->defaultCtx.history->threadId = "test-thread";
    agent->defaultCtx.identity.id = agentId;
    agent->defaultCtx.identity.parentId = "parent-agent";
    agent->defaultCtx.identity.friendlyName = friendlyName;
    agent->defaultCtx.state.currentStatus = AgentStatus::Idle;

    ON_CALL(*agent, getContext()).WillByDefault(ReturnRef(agent->defaultCtx));
    ON_CALL(*agent, getMutableContext())
        .WillByDefault(ReturnRef(agent->defaultCtx));
    ON_CALL(*agent, isInterrupted()).WillByDefault(Return(false));
    ON_CALL(*agent, isRunning()).WillByDefault(Return(false));
    ON_CALL(*agent, isBooting()).WillByDefault(Return(false));
    ON_CALL(*agent, run(_, _, _))
        .WillByDefault(Invoke([taskPromise, agentPtr](const std::string &task,
                                                      std::function<void(const StreamEvent &)>,
                                                      const std::vector<ImageContent> &) {
          AgentTurn turn;
          Message msg;
          msg.role = Role::Assistant;
          msg.content.push_back(TextContent{"worker-complete"});
          turn.messages.push_back(msg);
          agentPtr->defaultCtx.history->turns.push_back(turn);
          taskPromise->set_value(task);
        }));

    AgentRegistry::instance().registerAgent(agentId, agent);
    registeredAgentIds_.push_back(agentId);
    registeredAgents_.push_back(agent);
    return agent;
  }

  std::filesystem::path testPromptsDir;
  std::filesystem::path testHome_;
  std::string originalHome_;
  std::unique_ptr<ThreadManager> threadManager_;
  std::vector<std::string> registeredAgentIds_;
  std::vector<std::shared_ptr<IAgent>> registeredAgents_;
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

  ToolResult result = tool.execute(input, toolCtx);
  if (!result.success) {
    EXPECT_THAT(result.error,
                ::testing::Not(::testing::HasSubstr("Invalid persona")));
  }

  ::testing::Mock::VerifyAndClearExpectations(&agent);
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

TEST_F(SubagentToolTest, executorRetaskUsesChunkAwareDelegationContext) {
  const std::string threadId = createThread();
  const std::string planId = createPlanWithChunks(threadId);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto taskFuture = taskPromise->get_future();
  registerRetaskableAgent("executor-agent", "executor-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "executor";
  input.task = "Prioritize the parser ownership edge case.";
  input.agent_id = "executor-agent";
  input.plan_id = planId;
  input.chunk_id = "chunk-1";
  input.name = "executor-slot";
  input.title = "Executor Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  ASSERT_TRUE(result.success) << result.error;
  const std::string delegatedTask = taskFuture.get();
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("You are the executor responsible for exactly one assigned work chunk."));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Plan Title: Migrate work language"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Plan Objective: Ship chunk-aware delegation"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Plan Strategy Summary: Lead delegates by persisted chunk state"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk ID: chunk-1"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Chunk Title: Integrate delegation ownership"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Chunk Goal: Make executor delegation chunk-aware"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Chunk Context: The lead should wake executors from persisted chunk data"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Chunk Constraints: Do not expose sibling chunks"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Chunk Completion: Executor owns one chunk with bounded worker helpers"));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("The only chunk fields you may write are: status, attempt_count, result_summary."));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("Valid chunk_update payload pattern: {\"plan_id\":\"" +
                           planId +
                           "\",\"chunk_id\":\"chunk-1\",\"status\":\"Implemented\",\"attempt_count\":1,\"result_summary\":\"implemented scoped changes; verified with focused evidence\"}."));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("Do not send title, goal, context, constraints, completion, depends_on, assigned_agent_id, or review_summary through chunk_update."));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("Any design, review, dependency, or assignment fields in chunk_update will be rejected by runtime authority checks."));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("Only the lead accepts work and marks a chunk Done after review; your terminal success state is normally Implemented."));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("Your report must name the verification commands or tests you ran and the outcome."));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("If an anchor or local context is stale, reread and repair it before editing; do not guess."));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("Do not claim completion, verification, or review without evidence."));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("Changed: <files/behavior>"));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("Verified: <command/test and result>"));
  EXPECT_THAT(
      delegatedTask,
      ::testing::HasSubstr("Blockers/Risks: <none or concrete issue>"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("plan_id=\"" + planId + "\" and chunk_id=\"chunk-1\""));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Lead Notes\nPrioritize the parser ownership edge case."));
  EXPECT_THAT(delegatedTask,
              ::testing::Not(::testing::HasSubstr("Sibling chunk should stay hidden")));
  EXPECT_THAT(delegatedTask,
              ::testing::Not(::testing::HasSubstr("This should never appear in executor context")));

  const Plan updatedPlan = threadManager_->getPlan(threadId, planId);
  ASSERT_EQ(updatedPlan.chunks.size(), 2u);
  EXPECT_EQ(updatedPlan.chunks[0].assignedAgentId, "executor-agent");
  EXPECT_EQ(updatedPlan.chunks[0].status, WorkChunkStatus::InProgress);
  EXPECT_TRUE(updatedPlan.chunks[1].assignedAgentId.empty());
}

TEST_F(SubagentToolTest, workerRetaskStaysBoundedAndDoesNotClaimChunkOwnership) {
  const std::string threadId = createThread();
  const std::string planId = createPlanWithChunks(threadId);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto taskFuture = taskPromise->get_future();
  registerRetaskableAgent("worker-agent", "worker-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "worker";
  input.task = "Inspect parser fixtures and report only concrete failures.";
  input.agent_id = "worker-agent";
  input.plan_id = planId;
  input.chunk_id = "chunk-1";
  input.name = "worker-slot";
  input.title = "Worker Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId, "executor");
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  ASSERT_TRUE(result.success) << result.error;
  const std::string delegatedTask = taskFuture.get();
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("You are a worker helper supporting your parent executor on a bounded subtask."));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Subtask\nInspect parser fixtures and report only concrete failures."));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("You do not own a plan chunk."));
  EXPECT_THAT(delegatedTask,
              ::testing::Not(::testing::HasSubstr("Plan Title:")));
  EXPECT_THAT(delegatedTask,
              ::testing::Not(::testing::HasSubstr("Chunk Ownership Contract")));
  EXPECT_THAT(delegatedTask,
              ::testing::Not(::testing::HasSubstr("Integrate delegation ownership")));

  const Plan updatedPlan = threadManager_->getPlan(threadId, planId);
  EXPECT_TRUE(updatedPlan.chunks[0].assignedAgentId.empty());
}

TEST_F(SubagentToolTest, genericRetaskPreservesExistingSubagentFlow) {
  const std::string threadId = createThread();

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto taskFuture = taskPromise->get_future();
  registerRetaskableAgent("coder-agent", "coder-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Scan auth middleware usage.";
  input.agent_id = "coder-agent";
  input.name = "coder-slot";
  input.title = "Coder Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(taskFuture.get(), "Scan auth middleware usage.");
}

TEST_F(SubagentToolTest, executorRetaskFailsWhenChunkAlreadyOwnedByAnotherExecutor) {
  const std::string threadId = createThread();
  const std::string planId = createPlanWithChunks(threadId);
  Plan plan = threadManager_->getPlan(threadId, planId);
  plan.chunks[0].assignedAgentId = "different-executor";
  threadManager_->updatePlan(threadId, plan);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  registerRetaskableAgent("executor-agent", "executor-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "executor";
  input.task = "Take over chunk one.";
  input.agent_id = "executor-agent";
  input.plan_id = planId;
  input.chunk_id = "chunk-1";
  input.name = "executor-slot";
  input.title = "Executor Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error,
              ::testing::HasSubstr("already owned by executor agent"));
}

TEST_F(SubagentToolTest, executorDispatchFailsWhenDependencyIsNotDone) {
  const std::string threadId = createThread();
  const std::string planId =
      createPlanWithDependency(threadId, WorkChunkStatus::Verifying);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  registerRetaskableAgent("executor-agent", "executor-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "executor";
  input.task = "Do not start early.";
  input.agent_id = "executor-agent";
  input.plan_id = planId;
  input.chunk_id = "chunk-1";
  input.name = "executor-slot";
  input.title = "Executor Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error,
              ::testing::HasSubstr("status is Blocked"));
  EXPECT_THAT(result.error,
              ::testing::HasSubstr("dependencies must be Done"));

  const Plan updatedPlan = threadManager_->getPlan(threadId, planId);
  EXPECT_TRUE(updatedPlan.chunks[1].assignedAgentId.empty());
  EXPECT_EQ(updatedPlan.chunks[1].status, WorkChunkStatus::Blocked);
}

TEST_F(SubagentToolTest, executorDispatchSucceedsWhenDependenciesAreDone) {
  const std::string threadId = createThread();
  const std::string planId =
      createPlanWithDependency(threadId, WorkChunkStatus::Done);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto taskFuture = taskPromise->get_future();
  registerRetaskableAgent("executor-agent", "executor-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "executor";
  input.task = "Prerequisite is complete.";
  input.agent_id = "executor-agent";
  input.plan_id = planId;
  input.chunk_id = "chunk-1";
  input.name = "executor-slot";
  input.title = "Executor Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  ASSERT_TRUE(result.success) << result.error;
  EXPECT_THAT(taskFuture.get(), ::testing::HasSubstr("Chunk ID: chunk-1"));

  const Plan updatedPlan = threadManager_->getPlan(threadId, planId);
  EXPECT_EQ(updatedPlan.chunks[1].assignedAgentId, "executor-agent");
  EXPECT_EQ(updatedPlan.chunks[1].status, WorkChunkStatus::InProgress);
}

TEST_F(SubagentToolTest, executorRetaskFailsWhenAgentAlreadyOwnsAnotherChunk) {
  const std::string threadId = createThread();
  const std::string planId = createPlanWithChunks(threadId);
  Plan plan = threadManager_->getPlan(threadId, planId);
  plan.chunks[1].assignedAgentId = "executor-agent";
  threadManager_->updatePlan(threadId, plan);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  registerRetaskableAgent("executor-agent", "executor-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "executor";
  input.task = "Try to own a second chunk.";
  input.agent_id = "executor-agent";
  input.plan_id = planId;
  input.chunk_id = "chunk-1";
  input.name = "executor-slot";
  input.title = "Executor Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, ::testing::HasSubstr("already owns chunk"));
}

TEST_F(SubagentToolTest, workBoundPersonaRejectsLegacyRoleName) {
  const std::string threadId = createThread();
  const std::string planId = createPlanWithChunks(threadId);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "implementer";
  input.task = "Do the chunk.";
  input.plan_id = planId;
  input.chunk_id = "chunk-1";
  input.name = "legacy-slot";
  input.title = "Legacy Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error,
              ::testing::HasSubstr("legacy role 'implementer'; use 'executor'"));
}

TEST_F(SubagentToolTest, categoryOverrideRoutesRetaskModel) {
  const std::string threadId = createThread();
  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto agent = registerRetaskableAgent("coder-agent", "coder-slot", taskPromise);
  EXPECT_CALL(*agent, setModel("openai", "gpt-5-codex", "thinking")).Times(1);

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = "fallback";
  cfg.defaultModelId = "fallback-model";
  cfg.defaultModelVariant = "fallback-variant";
  cfg.modelRouterCategories["code"] = {"openai", "gpt-5-codex", "thinking"};
  cfg.purposeRoutes["coder"] = "other";
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Use explicit category.";
  input.agent_id = "coder-agent";
  input.category = "code";
  input.name = "coder-slot";
  input.title = "Coder Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));
  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"category\":\"code\""));
}

TEST_F(SubagentToolTest, purposeRouteAppliedWhenCategoryNotProvided) {
  const std::string threadId = createThread();
  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto agent =
      registerRetaskableAgent("coder-agent", "coder-slot", taskPromise);
  EXPECT_CALL(*agent, setModel("openrouter", "qwen-omni", "balanced")).Times(1);

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = "fallback";
  cfg.defaultModelId = "fallback-model";
  cfg.defaultModelVariant = "fallback-variant";
  cfg.modelRouterCategories["research"] = {"openrouter", "qwen-omni",
                                            "balanced"};
  cfg.purposeRoutes["coder"] = "research";
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Use persona route.";
  input.agent_id = "coder-agent";
  input.name = "coder-slot";
  input.title = "Coder Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));
  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"category\":\"research\""));
}

TEST_F(SubagentToolTest, missingCategoryFallsBackWithWarning) {
  const std::string threadId = createThread();
  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto agent =
      registerRetaskableAgent("coder-agent", "coder-slot", taskPromise);
  EXPECT_CALL(*agent, setModel("default-provider", "default-model", ""))
      .Times(1);

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = "default-provider";
  cfg.defaultModelId = "default-model";
  cfg.defaultModelVariant = "";
  cfg.modelRouterCategories.clear();
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Missing category route.";
  input.agent_id = "coder-agent";
  input.category = "does-not-exist";
  input.name = "coder-slot";
  input.title = "Coder Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));
  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_THAT(result.data, ::testing::HasSubstr("routing_warning"));
}
