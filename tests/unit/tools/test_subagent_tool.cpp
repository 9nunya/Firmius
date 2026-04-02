#include "AgentRegistry.hpp"
#include "IAgent.hpp"
#include "Engine.hpp"
#include "ConfigLoader.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "agents/PurposeLoader.hpp"
#include "persistence/ThreadManager.hpp"
#include "tools/SubagentTool.hpp"
#include "Context.hpp"
#include "providers/ProviderRegistry.hpp"
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
#include <stdexcept>

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
  MOCK_METHOD(ModelChoice, getPreferredModel, (), (const, override));
  MOCK_METHOD(void, interrupt, (), (override));
  MOCK_METHOD(bool, isInterrupted, (), (const, override));
  MOCK_METHOD(void, clearInterrupt, (), (override));
  MOCK_METHOD(void, compactNow,
              (std::function<void(const StreamEvent &)>), (override));
  MOCK_METHOD(void, saveHistory, (), (override));
  MOCK_METHOD(void, appendHistoryTurn, (const AgentTurn &), (override));
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

class NoSummaryProvider : public firmius::provider::IProvider {
public:
  explicit NoSummaryProvider(std::string providerId)
      : providerId_(std::move(providerId)) {}

  std::string getId() const override { return providerId_; }

  void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
              std::function<void(const StreamEvent&)> onEvent) override {
    callCount_.fetch_add(1);
    onEvent(StreamDone{StopReason::Stop});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = providerId_ + "-model";
    model.provider = getId();
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string&) override {
    return listModels().front();
  }

  void generateSummary(const std::string&, const AgentHistory&,
                       const std::string&,
                       std::function<void(const StreamEvent&)> onEvent,
                       std::atomic<bool>* = nullptr) override {
    onEvent(StreamDone{StopReason::Stop});
  }

  firmius::provider::ProviderType getProviderType() const override {
    return firmius::provider::ProviderType::APIKey;
  }

  int callCount() const { return callCount_.load(); }

private:
  std::string providerId_;
  std::atomic<int> callCount_{0};
};

class TextSummaryProvider : public firmius::provider::IProvider {
public:
  TextSummaryProvider(std::string providerId, std::string summary)
      : providerId_(std::move(providerId)), summary_(std::move(summary)) {}

  std::string getId() const override { return providerId_; }

  void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
              std::function<void(const StreamEvent&)> onEvent) override {
    callCount_.fetch_add(1);
    onEvent(TextChunk{summary_});
    onEvent(StreamDone{StopReason::Stop});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = providerId_ + "-model";
    model.provider = getId();
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string&) override {
    return listModels().front();
  }

  void generateSummary(const std::string&, const AgentHistory&,
                       const std::string&,
                       std::function<void(const StreamEvent&)> onEvent,
                       std::atomic<bool>* = nullptr) override {
    callCount_.fetch_add(1);
    onEvent(TextChunk{summary_});
    onEvent(StreamDone{StopReason::Stop});
  }

  firmius::provider::ProviderType getProviderType() const override {
    return firmius::provider::ProviderType::APIKey;
  }

  int callCount() const { return callCount_.load(); }

private:
  std::string providerId_;
  std::string summary_;
  std::atomic<int> callCount_{0};
};

class ErrorProvider : public firmius::provider::IProvider {
public:
  ErrorProvider(std::string providerId, std::string message, int code)
      : providerId_(std::move(providerId)),
        message_(std::move(message)),
        code_(code) {}

  std::string getId() const override { return providerId_; }

  void stream(const AgentHistory&, const firmius::provider::ProviderOptions&,
              std::function<void(const StreamEvent&)> onEvent) override {
    callCount_.fetch_add(1);
    onEvent(StreamError{message_, code_, ""});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = providerId_ + "-model";
    model.provider = getId();
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string&) override {
    return listModels().front();
  }

  void generateSummary(const std::string&, const AgentHistory&,
                       const std::string&,
                       std::function<void(const StreamEvent&)> onEvent,
                       std::atomic<bool>* = nullptr) override {
    onEvent(StreamError{message_, code_, ""});
  }

  firmius::provider::ProviderType getProviderType() const override {
    return firmius::provider::ProviderType::APIKey;
  }

  int callCount() const { return callCount_.load(); }

private:
  std::string providerId_;
  std::string message_;
  int code_ = 500;
  std::atomic<int> callCount_{0};
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

    std::ofstream auditorFile(testPromptsDir / "auditor.md");
    auditorFile << "---\nname: auditor\ntitle: Auditor\n---\nAuditor identity";
    auditorFile.close();

    std::ofstream customExecutorFile(testPromptsDir / "custom_executor.md");
    customExecutorFile << "---\nname: custom_executor\ntitle: Custom Executor\nwork_role: executor\n---\nCustom executor identity";
    customExecutorFile.close();

    std::ofstream customAuditorFile(testPromptsDir / "custom_auditor.md");
    customAuditorFile << "---\nname: custom_auditor\ntitle: Custom Auditor\nwork_role: auditor\n---\nCustom auditor identity";
    customAuditorFile.close();

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

  std::shared_ptr<NiceMock<MockAgent>>
  registerNoSummaryRetaskableAgent(const std::string &agentId,
                                   const std::string &friendlyName) {
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
        .WillByDefault(Invoke([agentPtr](const std::string &,
                                         std::function<void(const StreamEvent &)>,
                                         const std::vector<ImageContent> &) {
          agentPtr->defaultCtx.state.currentStatus = AgentStatus::Idle;
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

TEST_F(SubagentToolTest, customExecutorRoleUsesChunkAwareDelegationContext) {
  const std::string threadId = createThread();
  const std::string planId = createPlanWithChunks(threadId);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto taskFuture = taskPromise->get_future();
  registerRetaskableAgent("custom-executor-agent", "custom-executor-slot",
                          taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "custom_executor";
  input.task = "Prioritize the parser ownership edge case.";
  input.agent_id = "custom-executor-agent";
  input.plan_id = planId;
  input.chunk_id = "chunk-1";
  input.name = "custom-executor-slot";
  input.title = "Custom Executor Slot";
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
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk ID: chunk-1"));
  const Plan updatedPlan = threadManager_->getPlan(threadId, planId);
  EXPECT_EQ(updatedPlan.chunks[0].assignedAgentId, "custom-executor-agent");
  EXPECT_EQ(updatedPlan.chunks[0].status, WorkChunkStatus::InProgress);
}

TEST_F(SubagentToolTest, auditorRetaskBuildsChunkAwareReviewContext) {
  const std::string threadId = createThread();

  Plan plan;
  plan.threadId = threadId;
  plan.id = "plan-audit";
  plan.title = "Audit Plan";
  plan.objective = "Verify executor output";
  plan.strategy = "Auditor gets chunk-bound handoff";

  WorkChunk chunk;
  chunk.id = "chunk-audit";
  chunk.title = "Audit Target";
  chunk.goal = "Verify implementation";
  chunk.context = "Audit context";
  chunk.constraints = "No execution";
  chunk.completion = "Evidence-backed verdict";
  chunk.status = WorkChunkStatus::Implemented;
  chunk.assignedAgentId = "executor-agent";
  chunk.resultSummary = "Implemented scoped changes.";
  chunk.reviewSummary = "Preliminary review exists.";
  chunk.filesToRead = {"src/a.cpp", "include/a.hpp"};
  chunk.filesToTouch = {"src/a.cpp"};
  chunk.cwd = "/work/audit";
  chunk.verificationCondition = "All unit tests pass.";
  chunk.handoffNotes = "Focus on verification evidence.";

  WorkTask task1;
  task1.title = "Verify unit tests";
  task1.goal = "Run focused unit tests";
  task1.status = WorkChunkStatus::Implemented;
  WorkTask task2;
  task2.title = "Review diff";
  task2.notes = "Check for scope drift";
  task2.status = WorkChunkStatus::Ready;
  chunk.tasks = {task1, task2};

  plan.chunks = {chunk};
  threadManager_->writePlan(threadId, plan);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto taskFuture = taskPromise->get_future();
  registerRetaskableAgent("auditor-agent", "auditor-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "auditor";
  input.task = "Validate evidence and report verdict.";
  input.agent_id = "auditor-agent";
  input.plan_id = plan.id;
  input.chunk_id = chunk.id;
  input.name = "auditor-slot";
  input.title = "Auditor Slot";
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
              ::testing::HasSubstr("You are the auditor responsible for evidence-backed review"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Plan Title: Audit Plan"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Plan Objective: Verify executor output"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Plan Strategy Summary: Auditor gets chunk-bound handoff"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk ID: chunk-audit"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk Title: Audit Target"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk Goal: Verify implementation"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk Constraints: No execution"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk Completion: Evidence-backed verdict"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk Status: Implemented"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Assigned Executor: executor-agent"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Result Summary: Implemented scoped changes."));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Review Summary: Preliminary review exists."));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Files To Read: src/a.cpp, include/a.hpp"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Files To Touch: src/a.cpp"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Working Directory: /work/audit"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Verification Condition: All unit tests pass."));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Handoff Notes: Focus on verification evidence."));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk Tasks"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Verdict: <accept/reject/needs-evidence>"));
  EXPECT_THAT(delegatedTask,
              ::testing::HasSubstr("Lead Notes\nValidate evidence and report verdict."));
}

TEST_F(SubagentToolTest, customAuditorRoleBuildsChunkAwareReviewContext) {
  const std::string threadId = createThread();

  Plan plan;
  plan.threadId = threadId;
  plan.id = "plan-custom-audit";
  plan.title = "Audit Plan";
  plan.objective = "Verify executor output";
  plan.strategy = "Auditor gets chunk-bound handoff";

  WorkChunk chunk;
  chunk.id = "chunk-custom-audit";
  chunk.title = "Audit Target";
  chunk.goal = "Verify implementation";
  chunk.context = "Audit context";
  chunk.constraints = "No execution";
  chunk.completion = "Evidence-backed verdict";
  chunk.status = WorkChunkStatus::Implemented;
  chunk.assignedAgentId = "executor-agent";
  plan.chunks = {chunk};
  threadManager_->writePlan(threadId, plan);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto taskFuture = taskPromise->get_future();
  registerRetaskableAgent("custom-auditor-agent", "custom-auditor-slot",
                          taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "custom_auditor";
  input.task = "Validate evidence and report verdict.";
  input.agent_id = "custom-auditor-agent";
  input.plan_id = plan.id;
  input.chunk_id = chunk.id;
  input.name = "custom-auditor-slot";
  input.title = "Custom Auditor Slot";
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
              ::testing::HasSubstr("You are the auditor responsible for evidence-backed review"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk ID: chunk-custom-audit"));
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
  EXPECT_THAT(result.error,
              ::testing::HasSubstr("dependency 'dep-1'"));

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

TEST_F(SubagentToolTest, explicitCategoryHonoredWhenUserRequestedInHistory) {
  const std::string threadId = createThread();
  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto agent = registerRetaskableAgent("coder-agent", "coder-slot", taskPromise);
  EXPECT_CALL(*agent, setModel("openai", "gpt-5-codex", "thinking")).Times(1);

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = "fallback";
  cfg.defaultModelId = "fallback-model";
  cfg.defaultModelVariant = "fallback-variant";
  cfg.modelRouterCategories["gemini-fast"] = {"openai", "gpt-5-codex",
                                               "thinking"};
  cfg.modelRouterCategories["research"] = {"openrouter", "qwen-omni",
                                            "balanced"};
  cfg.purposeRoutes["coder"] = "research";
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Use explicit category.";
  input.agent_id = "coder-agent";
  input.category = "gemini-fast";
  input.name = "coder-slot";
  input.title = "Coder Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  AgentTurn userTurn;
  Message userMessage;
  userMessage.role = Role::User;
  userMessage.content.push_back(
      TextContent{"Please use the gemini-fast route for this scout."});
  userTurn.messages.push_back(userMessage);
  ctx_obj.history->turns.push_back(userTurn);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));
  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_THAT(result.data,
              ::testing::HasSubstr("\"category\":\"gemini-fast\""));
  EXPECT_THAT(result.data,
              ::testing::Not(::testing::HasSubstr("routing_warning")));
}

TEST_F(SubagentToolTest, explicitCategoryIgnoredWhenUserDidNotRequestOverride) {
  const std::string threadId = createThread();
  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto agent = registerRetaskableAgent("coder-agent", "coder-slot", taskPromise);
  EXPECT_CALL(*agent, setModel("openrouter", "qwen-omni", "balanced")).Times(1);

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = "fallback";
  cfg.defaultModelId = "fallback-model";
  cfg.defaultModelVariant = "fallback-variant";
  cfg.modelRouterCategories["gemini-fast"] = {"antigravity", "gemini-3-flash",
                                               ""};
  cfg.modelRouterCategories["research"] = {"openrouter", "qwen-omni",
                                            "balanced"};
  cfg.purposeRoutes["coder"] = "research";
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Analyze the repository.";
  input.agent_id = "coder-agent";
  input.category = "gemini-fast";
  input.name = "coder-slot";
  input.title = "Coder Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  AgentTurn userTurn;
  Message userMessage;
  userMessage.role = Role::User;
  userMessage.content.push_back(TextContent{"Please analyze the repository."});
  userTurn.messages.push_back(userMessage);
  ctx_obj.history->turns.push_back(userTurn);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));
  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"category\":\"research\""));
  EXPECT_THAT(result.data, ::testing::HasSubstr("routing_warning"));
  EXPECT_THAT(
      result.data,
      ::testing::HasSubstr("Ignored explicit category 'gemini-fast'"));
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

TEST_F(SubagentToolTest, updatedPurposeRouteAppliesOnNextExecution) {
  const std::string threadId = createThread();
  auto agent = std::make_shared<NiceMock<MockAgent>>();
  auto* agentPtr = agent.get();
  agent->defaultCtx.history = std::make_shared<AgentHistory>();
  agent->defaultCtx.history->threadId = threadId;
  agent->defaultCtx.identity.id = "coder-agent";
  agent->defaultCtx.identity.parentId = "parent-agent";
  agent->defaultCtx.identity.friendlyName = "coder-slot";
  agent->defaultCtx.state.currentStatus = AgentStatus::Idle;

  ON_CALL(*agent, getContext()).WillByDefault(ReturnRef(agent->defaultCtx));
  ON_CALL(*agent, getMutableContext())
      .WillByDefault(ReturnRef(agent->defaultCtx));
  ON_CALL(*agent, isInterrupted()).WillByDefault(Return(false));
  ON_CALL(*agent, isRunning()).WillByDefault(Return(false));
  ON_CALL(*agent, isBooting()).WillByDefault(Return(false));
  ON_CALL(*agent, run(_, _, _))
      .WillByDefault(Invoke([agentPtr](const std::string &,
                                       std::function<void(const StreamEvent &)>,
                                       const std::vector<ImageContent> &) {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        agentPtr->defaultCtx.state.currentStatus = AgentStatus::Idle;
      }));

  AgentRegistry::instance().registerAgent("coder-agent", agent);
  registeredAgentIds_.push_back("coder-agent");
  registeredAgents_.push_back(agent);
  {
    ::testing::InSequence seq;
    EXPECT_CALL(*agent, setModel("provider-a", "model-a", "balanced")).Times(1);
    EXPECT_CALL(*agent, setModel("provider-b", "model-b", "high")).Times(1);
  }

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = "fallback";
  cfg.defaultModelId = "fallback-model";
  cfg.defaultModelVariant = "";
  cfg.modelRouterCategories.clear();
  cfg.modelRouterCategories["research"] = {"provider-a", "model-a",
                                            "balanced"};
  cfg.purposeRoutes["coder"] = "research";
  cfg.enableSubagentRouteFallback = false;
  cfg.subagentRouteFallbackOrder.clear();
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Use current purpose route.";
  input.agent_id = "coder-agent";
  input.name = "coder-slot";
  input.title = "Coder Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));
  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult first = tool.execute(input, toolCtx);
  ASSERT_TRUE(first.success) << first.error;
  EXPECT_THAT(first.data, ::testing::HasSubstr("\"category\":\"research\""));
  std::this_thread::sleep_for(std::chrono::milliseconds(450));

  cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.modelRouterCategories["review"] = {"provider-b", "model-b", "high"};
  cfg.purposeRoutes["coder"] = "review";
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  ToolResult second = tool.execute(input, toolCtx);
  ASSERT_TRUE(second.success) << second.error;
  EXPECT_THAT(second.data, ::testing::HasSubstr("\"category\":\"review\""));
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

TEST_F(SubagentToolTest, routeFallbackRetriesNextCategoryWhenEnabled) {
  const std::string threadId = createThread();
  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto agent = registerRetaskableAgent("coder-agent", "coder-slot", taskPromise);

  EXPECT_CALL(*agent, setModel("bad-provider", "bad-model", ""))
      .WillOnce(::testing::Throw(std::runtime_error("provider unavailable")));
  EXPECT_CALL(*agent, setModel("good-provider", "good-model", "")).Times(1);

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = "default-provider";
  cfg.defaultModelId = "default-model";
  cfg.modelRouterCategories["primary"] = {"bad-provider", "bad-model", ""};
  cfg.modelRouterCategories["fallback"] = {"good-provider", "good-model", ""};
  cfg.enableSubagentRouteFallback = true;
  cfg.subagentRouteFallbackOrder = {"fallback"};
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Use fallback route.";
  input.agent_id = "coder-agent";
  input.category = "primary";
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
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"fallback_used\":true"));
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"category\":\"fallback\""));
}

TEST_F(SubagentToolTest, routeFallbackCanBeDisabled) {
  const std::string threadId = createThread();
  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto agent = registerRetaskableAgent("coder-agent", "coder-slot", taskPromise);

  EXPECT_CALL(*agent, setModel("bad-provider", "bad-model", ""))
      .WillOnce(::testing::Throw(std::runtime_error("provider unavailable")));
  EXPECT_CALL(*agent, setModel("good-provider", "good-model", "")).Times(0);

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = "default-provider";
  cfg.defaultModelId = "default-model";
  cfg.modelRouterCategories["primary"] = {"bad-provider", "bad-model", ""};
  cfg.modelRouterCategories["fallback"] = {"good-provider", "good-model", ""};
  cfg.enableSubagentRouteFallback = false;
  cfg.subagentRouteFallbackOrder = {"fallback"};
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Do not fallback.";
  input.agent_id = "coder-agent";
  input.category = "primary";
  input.name = "coder-slot";
  input.title = "Coder Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));
  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);
  EXPECT_FALSE(result.success);
}

TEST_F(SubagentToolTest, spawnedRouteFallbackRetriesOnNoUsableSummary) {
  const std::string threadId = createThread();
  auto primary = std::make_shared<NoSummaryProvider>("spawn-no-summary-provider");
  auto fallback =
      std::make_shared<TextSummaryProvider>("spawn-summary-provider",
                                            "fallback summary");
  firmius::provider::ProviderRegistry::instance().registerProvider(primary);
  firmius::provider::ProviderRegistry::instance().registerProvider(fallback);

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = primary->getId();
  cfg.defaultModelId = primary->listModels().front().id;
  cfg.modelRouterCategories["primary"] = {primary->getId(),
                                           primary->listModels().front().id, ""};
  cfg.modelRouterCategories["fallback"] = {fallback->getId(),
                                           fallback->listModels().front().id, ""};
  cfg.enableSubagentRouteFallback = true;
  cfg.subagentRouteFallbackOrder = {"fallback"};
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Retry on no summary.";
  input.category = "primary";
  input.name = "spawn-slot";
  input.title = "Spawn Slot";

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(primary->callCount(), 1);
  EXPECT_EQ(fallback->callCount(), 1);
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"fallback_used\":true"));
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"category\":\"fallback\""));
  EXPECT_THAT(result.data, ::testing::HasSubstr("fallback summary"));
}

TEST_F(SubagentToolTest, spawnedAsyncRouteFallbackRetriesOnImmediateFailure) {
  const std::string threadId = createThread();
  auto primary = std::make_shared<ErrorProvider>("spawn-error-provider",
                                                 "quota exhausted", 429);
  auto fallback =
      std::make_shared<TextSummaryProvider>("spawn-summary-provider",
                                            "fallback summary");
  firmius::provider::ProviderRegistry::instance().registerProvider(primary);
  firmius::provider::ProviderRegistry::instance().registerProvider(fallback);

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = primary->getId();
  cfg.defaultModelId = primary->listModels().front().id;
  cfg.modelRouterCategories["primary"] = {primary->getId(),
                                          primary->listModels().front().id, ""};
  cfg.modelRouterCategories["fallback"] = {fallback->getId(),
                                           fallback->listModels().front().id, ""};
  cfg.enableSubagentRouteFallback = true;
  cfg.subagentRouteFallbackOrder = {"fallback"};
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Retry on immediate stream error.";
  input.category = "primary";
  input.name = "spawn-async-slot";
  input.title = "Spawn Async Slot";
  input.async = true;

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(primary->callCount(), 1);
  EXPECT_EQ(fallback->callCount(), 1);
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"fallback_used\":true"));
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"category\":\"fallback\""));
}

TEST_F(SubagentToolTest, retaskedRouteFallbackRetriesOnNoUsableSummary) {
  const std::string threadId = createThread();
  auto primary = std::make_shared<NoSummaryProvider>("retask-no-summary-provider");
  auto fallback =
      std::make_shared<TextSummaryProvider>("retask-summary-provider",
                                            "fallback summary");
  firmius::provider::ProviderRegistry::instance().registerProvider(primary);
  firmius::provider::ProviderRegistry::instance().registerProvider(fallback);

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = primary->getId();
  cfg.defaultModelId = primary->listModels().front().id;
  cfg.modelRouterCategories["primary"] = {primary->getId(),
                                           primary->listModels().front().id, ""};
  cfg.modelRouterCategories["fallback"] = {fallback->getId(),
                                           fallback->listModels().front().id, ""};
  cfg.enableSubagentRouteFallback = true;
  cfg.subagentRouteFallbackOrder = {"fallback"};
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  auto agent = registerNoSummaryRetaskableAgent("coder-agent", "coder-slot");
  EXPECT_CALL(*agent,
              setModel(primary->getId(), primary->listModels().front().id, ""))
      .Times(1);
  EXPECT_CALL(*agent,
              setModel(fallback->getId(), fallback->listModels().front().id, ""))
      .Times(1);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Retry on no summary.";
  input.agent_id = "coder-agent";
  input.category = "primary";
  input.name = "coder-slot";
  input.title = "Coder Slot";

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));

  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  ToolResult result = tool.execute(input, toolCtx);

  ASSERT_TRUE(result.success) << result.error;
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"fallback_used\":true"));
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"category\":\"fallback\""));
}

TEST_F(SubagentToolTest, cancelledRetaskDoesNotTriggerFallbackRouteClone) {
  const std::string threadId = createThread();
  auto agent = registerRetaskableAgent("cancel-agent", "cancel-slot",
                                       std::make_shared<std::promise<std::string>>());

  auto started = std::make_shared<std::promise<void>>();
  auto startedFuture = started->get_future().share();

  EXPECT_CALL(*agent, run(_, _, _))
      .WillOnce(Invoke([agentPtr = agent.get(), started](const std::string &,
                                                         std::function<void(const StreamEvent &)>,
                                                         const std::vector<ImageContent> &) {
        agentPtr->defaultCtx.state.currentStatus = AgentStatus::Streaming;
        started->set_value();
        while (agentPtr->defaultCtx.state.currentStatus != AgentStatus::Cancelled) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        AgentTurn turn;
        turn.turnId = "cancelled-" + std::to_string(agentPtr->defaultCtx.history->turns.size());
        Message message;
        message.role = Role::System;
        message.visibility = MessageVisibility::Visible;
        message.content.push_back(NoticeContent{
            "Agent Cancelled",
            "The agent execution was interrupted.",
            "Execution stopped before completion and can be resumed.",
            NoticeSeverity::Warning});
        message.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        turn.messages.push_back(message);
        agentPtr->defaultCtx.history->turns.push_back(turn);
        agentPtr->defaultCtx.state.currentStatus = AgentStatus::Cancelled;
      }));
  ON_CALL(*agent, interrupt()).WillByDefault(Invoke([agentPtr = agent.get()]() {
    agentPtr->defaultCtx.state.currentStatus = AgentStatus::Cancelled;
  }));
  ON_CALL(*agent, isInterrupted())
      .WillByDefault(Invoke([agentPtr = agent.get()]() {
        return agentPtr->defaultCtx.state.currentStatus ==
               AgentStatus::Cancelled;
      }));

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Cancel this run and do not clone a fallback child.";
  input.agent_id = "cancel-agent";
  input.category = "primary";
  input.name = "cancel-slot";
  input.title = "Cancel Slot";
  input.async = false;

  auto cfg = firmius::shared::ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = "default-provider";
  cfg.defaultModelId = "default-model";
  cfg.modelRouterCategories["primary"] = {"good-provider", "good-model", ""};
  cfg.modelRouterCategories["fallback"] = {"fallback-provider",
                                           "fallback-model", ""};
  cfg.enableSubagentRouteFallback = true;
  cfg.subagentRouteFallbackOrder = {"fallback"};
  firmius::shared::ConfigLoader::instance().updateConfig(cfg);

  EXPECT_CALL(*agent, setModel("good-provider", "good-model", ""))
      .Times(1);
  EXPECT_CALL(*agent, setModel("fallback-provider", "fallback-model", ""))
      .Times(0);

  MockAgent parent;
  AgentContext ctx_obj = makeParentContext(threadId);
  EXPECT_CALL(parent, getContext()).WillRepeatedly(ReturnRef(ctx_obj));
  NiceMock<MockHost> host;
  ToolContext toolCtx{host, parent, "test-call-id"};

  std::thread canceller([&]() {
    startedFuture.wait();
    Engine::instance().cancelAgent("cancel-agent");
  });

  ToolResult result = tool.execute(input, toolCtx);
  canceller.join();

  ASSERT_TRUE(result.success) << result.error;
  EXPECT_THAT(result.data, ::testing::HasSubstr("\"status\":\"cancelled\""));
  EXPECT_THAT(result.data, ::testing::Not(::testing::HasSubstr("fallback_used\":true")));
}

TEST_F(SubagentToolTest, executorHandoffIncludesV2RichSpecFields) {
  const std::string threadId = createThread();
  
  // Create plan with V2 rich spec fields
  Plan plan;
  plan.threadId = threadId;
  plan.id = "plan-v2";
  plan.title = "V2 Integration Test";
  plan.objective = "Test V2 field handoff";
  plan.strategy = "Lead uses rich chunk specs";

  WorkChunk chunk;
  chunk.id = "chunk-v2";
  chunk.title = "V2 Rich Spec Chunk";
  chunk.goal = "Test V2 field persistence and handoff";
  chunk.context = "Integration test for V2 work language";
  chunk.constraints = "Must include all V2 fields";
  chunk.completion = "V2 fields present in executor handoff";
  chunk.status = WorkChunkStatus::Ready;
  
  // V2 rich spec fields
  chunk.filesToRead = {"src/parser/interface.hpp", "src/lexer/spec.txt"};
  chunk.filesToTouch = {"src/parser/impl.cpp", "src/lexer/impl.cpp"};
  chunk.cwd = "/work/language";
  chunk.verificationCondition = "Compiler pipeline builds, unit tests pass, and parser/lexer integration works";
  chunk.handoffNotes = "Focus on correctness over optimization. Edge cases in anchor handling.";

  plan.chunks = {chunk};
  threadManager_->writePlan(threadId, plan);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto taskFuture = taskPromise->get_future();
  registerRetaskableAgent("executor-agent", "executor-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "executor";
  input.task = "Implement V2 chunk with rich spec.";
  input.agent_id = "executor-agent";
  input.plan_id = "plan-v2";
  input.chunk_id = "chunk-v2";
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
  
  // Verify V2 rich spec fields are present in executor handoff
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Files To Read:"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("src/parser/interface.hpp"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("src/lexer/spec.txt"));
  
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Files To Touch:"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("src/parser/impl.cpp"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("src/lexer/impl.cpp"));
  
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Working Directory:"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("/work/language"));
  
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Verification Condition:"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Compiler pipeline builds, unit tests pass"));

  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Handoff Notes:"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Focus on correctness over optimization"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Edge cases in anchor handling"));
}

TEST_F(SubagentToolTest, executorHandoffIncludesChunkTasks) {
  const std::string threadId = createThread();
  
  // Create plan with task-bearing chunk
  Plan plan;
  plan.threadId = threadId;
  plan.id = "plan-tasks";
  plan.title = "Task-Bearing Chunk Test";
  plan.objective = "Test task handoff";
  plan.strategy = "Lead uses task-bearing chunks";

  WorkChunk chunk;
  chunk.id = "chunk-tasks";
  chunk.title = "Core Infrastructure";
  chunk.goal = "Implement core language infrastructure";
  chunk.context = "Multi-surface implementation";
  chunk.constraints = "No external dependencies";
  chunk.completion = "All tasks complete";
  chunk.status = WorkChunkStatus::Ready;
  
  // V2 task structure
  WorkTask task1;
  task1.id = "task-lexer";
  task1.title = "Lexer impl";
  task1.goal = "Implement lexer with token types and stream output";
  task1.status = WorkChunkStatus::Done;
  task1.notes = "Use existing token definitions";
  task1.verificationCondition = "Lexer produces correct token stream";
  
  WorkTask task2;
  task2.id = "task-parser";
  task2.title = "AST+Parser impl";
  task2.goal = "Define AST nodes and implement parser";
  task2.status = WorkChunkStatus::InProgress;
  
  WorkTask task3;
  task3.id = "task-visitor";
  task3.title = "Visitor + Compiler";
  task3.goal = "Implement visitor pattern and compilation";
  task3.status = WorkChunkStatus::Ready;
  task3.verificationCondition = "Compiler pipeline builds and unit tests pass";

  chunk.tasks = {task1, task2, task3};
  plan.chunks = {chunk};
  threadManager_->writePlan(threadId, plan);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto taskFuture = taskPromise->get_future();
  registerRetaskableAgent("executor-agent", "executor-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "executor";
  input.task = "Implement task-bearing chunk.";
  input.agent_id = "executor-agent";
  input.plan_id = "plan-tasks";
  input.chunk_id = "chunk-tasks";
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
  
  // Verify chunk tasks section is present
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Chunk Tasks"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("3 internal tasks"));
  
  // Verify task titles appear
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Lexer impl"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("AST+Parser impl"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Visitor + Compiler"));
  
  // Verify task goals appear
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Implement lexer with token types"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Define AST nodes and implement parser"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Implement visitor pattern"));
  
  // Verify task statuses appear
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[Done]"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[InProgress]"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[Ready]"));
  
  // Verify task notes appear when present
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Note:"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Use existing token definitions"));
  
  // Verify task verification conditions appear when present
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Verify:"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Lexer produces correct token stream"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Compiler pipeline builds and unit tests pass"));
}

TEST_F(SubagentToolTest, executorHandoffPreservesAllTaskStatuses) {
  const std::string threadId = createThread();
  
  // Create plan with task-bearing chunk using ALL status values
  Plan plan;
  plan.threadId = threadId;
  plan.id = "plan-all-statuses";
  plan.title = "All Statuses Test";
  plan.objective = "Test all task status rendering";
  plan.strategy = "Test full WorkChunkStatus enum";

  WorkChunk chunk;
  chunk.id = "chunk-all-statuses";
  chunk.title = "Full Status Coverage";
  chunk.goal = "Test all task statuses";
  chunk.context = "Regression test for status rendering";
  chunk.constraints = "None";
  chunk.completion = "All statuses rendered truthfully";
  chunk.status = WorkChunkStatus::Ready;
  
  // Tasks covering statuses that were previously mislabeled
  WorkTask taskReady;
  taskReady.id = "task-ready";
  taskReady.title = "Ready Task";
  taskReady.goal = "Not started yet";
  taskReady.status = WorkChunkStatus::Ready;
  
  WorkTask taskImplemented;
  taskImplemented.id = "task-implemented";
  taskImplemented.title = "Implemented Task";
  taskImplemented.goal = "Implementation complete, awaiting verification";
  taskImplemented.status = WorkChunkStatus::Implemented;
  
  WorkTask taskVerifying;
  taskVerifying.id = "task-verifying";
  taskVerifying.title = "Verifying Task";
  taskVerifying.goal = "Running verification now";
  taskVerifying.status = WorkChunkStatus::Verifying;
  
  WorkTask taskCancelled;
  taskCancelled.id = "task-cancelled";
  taskCancelled.title = "Cancelled Task";
  taskCancelled.goal = "This task was cancelled";
  taskCancelled.status = WorkChunkStatus::Cancelled;

  chunk.tasks = {taskReady, taskImplemented, taskVerifying, taskCancelled};
  plan.chunks = {chunk};
  threadManager_->writePlan(threadId, plan);

  auto taskPromise = std::make_shared<std::promise<std::string>>();
  auto taskFuture = taskPromise->get_future();
  registerRetaskableAgent("executor-agent", "executor-slot", taskPromise);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "executor";
  input.task = "Test all statuses.";
  input.agent_id = "executor-agent";
  input.plan_id = "plan-all-statuses";
  input.chunk_id = "chunk-all-statuses";
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
  
  // Verify all task statuses are rendered truthfully (not collapsed to Ready)
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[Ready]"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[Implemented]"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[Verifying]"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[Cancelled]"));
  
  // Verify task titles appear with correct statuses
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[Ready] Ready Task"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[Implemented] Implemented Task"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[Verifying] Verifying Task"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("[Cancelled] Cancelled Task"));
  
  // Verify task goals appear
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Not started yet"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Implementation complete, awaiting verification"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("Running verification now"));
  EXPECT_THAT(delegatedTask, ::testing::HasSubstr("This task was cancelled"));
}
