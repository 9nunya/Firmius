#include "tools/ChunkAddTool.hpp"
#include "tools/ChunkGetTool.hpp"
#include "tools/ChunkListTool.hpp"
#include "tools/ChunkReadyForExecutionTool.hpp"
#include "tools/ChunkUpdateTool.hpp"
#include "tools/PlanCreateTool.hpp"
#include "tools/PlanGetTool.hpp"
#include "tools/PlanListTool.hpp"
#include "tools/PlanSetActiveTool.hpp"
#include "tools/PlanUpdateTool.hpp"
#include "tools/ToolRegistry.hpp"
#include "IAgent.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include "../mocks/MockEnvironment.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <rapidjson/document.h>
#include <type_traits>
#include <variant>
#include <vector>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

class TestAgent : public IAgent {
public:
  TestAgent() {
    context_.history = std::make_shared<AgentHistory>();
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
  std::shared_ptr<IEnvironment> getEnvironment() const override {
    return environment_;
  }
  std::shared_ptr<IPermissions> getPermissions() const override {
    return permissions_;
  }
  std::shared_ptr<firmius::shared::IHost> getHost() override {
    return environment_->getHost();
  }

  AgentContext context_;
  std::shared_ptr<firmius::test::MockEnvironment> environment_;
  std::shared_ptr<firmius::test::MockPermissions> permissions_;
};

rapidjson::Value jsonString(const std::string &value,
                            rapidjson::Document::AllocatorType &alloc) {
  return rapidjson::Value(value.c_str(), alloc);
}

rapidjson::Document parseJson(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  return doc;
}

class WorkToolsTest : public ::testing::Test {
protected:
  void SetUp() override {
    char tempTemplate[] = "/tmp/firmius_work_tools_XXXXXX";
    char *result = mkdtemp(tempTemplate);
    ASSERT_NE(result, nullptr);
    tempDir_ = result;

    const char *existingHome = std::getenv("HOME");
    if (existingHome) {
      originalHome_ = existingHome;
    }
    setenv("HOME", tempDir_.c_str(), 1);
    std::filesystem::create_directories(tempDir_ + "/.firmius/threads");
    Harness::instance().init();

    threadManager_ =
        std::make_unique<ThreadManager>(tempDir_ + "/.firmius/threads");
    ThreadMetadata metadata;
    metadata.title = "Thread";
    metadata.hostOptions.type = HostType::Local;
    metadata.cwd = "/tmp/work";
    metadata.leadPersona = "lead";
    threadId_ = threadManager_->createThread(metadata);

    agent_.context_.permissions.allowedScopes = {
        ToolScope::Semantic,  ToolScope::PlanRead, ToolScope::PlanWrite,
        ToolScope::ChunkRead, ToolScope::ChunkWrite, ToolScope::ChunkReview};
    agent_.context_.history->threadId = threadId_;
    agent_.context_.environment.cwd = "/tmp/work";
    agent_.context_.config.personaName = "lead";
    agent_.context_.identity.id = "lead-agent";

    registerTools();
  }

  void TearDown() override {
    Harness::instance().shutdown();
    if (!originalHome_.empty()) {
      setenv("HOME", originalHome_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
    std::filesystem::remove_all(tempDir_);
  }

  void registerTools() {
    registry_.registerTool(std::make_unique<PlanCreateTool>());
    registry_.registerTool(std::make_unique<PlanListTool>());
    registry_.registerTool(std::make_unique<PlanGetTool>());
    registry_.registerTool(std::make_unique<PlanUpdateTool>());
    registry_.registerTool(std::make_unique<PlanSetActiveTool>());
    registry_.registerTool(std::make_unique<ChunkAddTool>());
    registry_.registerTool(std::make_unique<ChunkListTool>());
    registry_.registerTool(std::make_unique<ChunkGetTool>());
    registry_.registerTool(std::make_unique<ChunkUpdateTool>());
    registry_.registerTool(std::make_unique<ChunkReadyForExecutionTool>());
  }

  ToolResult execute(const std::string &toolName, rapidjson::Document &input) {
    ToolContext ctx{*agent_.getHost(), agent_, "tool-call"};
    return registry_.execute(toolName, input, ctx);
  }

  std::vector<AppEvent>
  captureEvents(const std::function<void()> &action) {
    std::vector<AppEvent> events;
    const int subId = Harness::instance().subscribe(
        [&events](const AppEvent &event) { events.push_back(event); });
    action();
    Harness::instance().unsubscribe(subId);
    return events;
  }

  template <typename T>
  const T *findEvent(const std::vector<AppEvent> &events) const {
    for (const auto &event : events) {
      if (const auto *typed = std::get_if<T>(&event)) {
        return typed;
      }
    }
    return nullptr;
  }

  template <typename T>
  size_t countEvents(const std::vector<AppEvent> &events) const {
    size_t count = 0;
    for (const auto &event : events) {
      if (std::holds_alternative<T>(event)) {
        ++count;
      }
    }
    return count;
  }

  std::string createPlanDirect(const std::string &title = "Plan A") {
    Plan plan;
    plan.threadId = threadId_;
    plan.title = title;
    plan.objective = "Ship tool APIs";
    plan.context = "Chunk 2";
    plan.strategy = "Persist boring tool operations";
    plan.notes = "notes";
    return threadManager_->createPlan(plan);
  }

  std::string addChunkDirect(const std::string &planId, const std::string &chunkId,
                             WorkChunkStatus status = WorkChunkStatus::Ready,
                             std::vector<std::string> dependsOn = {}) {
    Plan plan = threadManager_->getPlan(threadId_, planId);
    WorkChunk chunk;
    chunk.id = chunkId;
    chunk.title = "Chunk " + chunkId;
    chunk.goal = "Goal";
    chunk.context = "Context";
    chunk.constraints = "Constraints";
    chunk.completion = "Completion";
    chunk.status = status;
    chunk.dependsOn = std::move(dependsOn);
    chunk.createdAt = 1;
    chunk.updatedAt = 1;
    plan.chunks.push_back(chunk);
    threadManager_->updatePlan(threadId_, plan);
    return chunk.id;
  }

  void setAgentRole(const std::string &persona,
                    std::vector<ToolScope> scopes,
                    const std::string &agentId) {
    agent_.context_.config.personaName = persona;
    agent_.context_.permissions.allowedScopes = std::move(scopes);
    agent_.context_.identity.id = agentId;
  }

  rapidjson::Document makeObject(
      const std::map<std::string, std::string> &strings = {},
      const std::map<std::string, int> &ints = {},
      const std::map<std::string, bool> &bools = {},
      const std::map<std::string, std::vector<std::string>> &arrays = {}) {
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    for (const auto &[key, value] : strings) {
      doc.AddMember(jsonString(key, alloc), jsonString(value, alloc), alloc);
    }
    for (const auto &[key, value] : ints) {
      doc.AddMember(jsonString(key, alloc), rapidjson::Value(value), alloc);
    }
    for (const auto &[key, value] : bools) {
      doc.AddMember(jsonString(key, alloc), rapidjson::Value(value), alloc);
    }
    for (const auto &[key, values] : arrays) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto &value : values) {
        arr.PushBack(jsonString(value, alloc), alloc);
      }
      doc.AddMember(jsonString(key, alloc), arr, alloc);
    }
    return doc;
  }

  std::string tempDir_;
  std::string originalHome_;
  std::string threadId_;
  std::unique_ptr<ThreadManager> threadManager_;
  ToolRegistry registry_;
  TestAgent agent_;
};

TEST_F(WorkToolsTest, planCreatePersistsAndSetsActiveByDefault) {
  auto input = makeObject({{"title", "Plan 1"},
                           {"objective", "Do work"},
                           {"context", "Context"},
                           {"strategy", "Strategy"}});

  ToolResult result;
  const auto events = captureEvents([&]() { result = execute("plan_create", input); });
  ASSERT_TRUE(result.success) << result.error;

  auto doc = parseJson(result.data);
  const std::string planId = doc["plan_id"].GetString();
  EXPECT_EQ(doc["status"].GetString(), std::string("Active"));
  EXPECT_TRUE(doc["active"].GetBool());

  const auto metadata = threadManager_->getMetadata(threadId_);
  EXPECT_EQ(metadata.activePlanId, planId);

  const auto plan = threadManager_->getPlan(threadId_, planId);
  EXPECT_EQ(plan.title, "Plan 1");
  EXPECT_EQ(plan.status, PlanStatus::Active);

  const auto *created = findEvent<PlanCreated>(events);
  ASSERT_NE(created, nullptr);
  EXPECT_EQ(created->threadId, threadId_);
  EXPECT_EQ(created->plan.id, planId);
  EXPECT_EQ(created->plan.title, "Plan 1");
}

TEST_F(WorkToolsTest, planListShowsPersistedPlansAndActiveState) {
  const std::string activePlanId = createPlanDirect("Active Plan");
  const std::string draftPlanId = createPlanDirect("Draft Plan");
  auto metadata = threadManager_->getMetadata(threadId_);
  metadata.activePlanId = activePlanId;
  threadManager_->updateMetadata(threadId_, metadata);

  auto input = makeObject();
  auto result = execute("plan_list", input);
  ASSERT_TRUE(result.success) << result.error;

  auto doc = parseJson(result.data);
  ASSERT_TRUE(doc.IsArray());
  ASSERT_EQ(doc.Size(), 2u);
  bool sawActive = false;
  bool sawDraft = false;
  for (const auto &plan : doc.GetArray()) {
    const std::string planId = plan["plan_id"].GetString();
    if (planId == activePlanId) {
      sawActive = true;
      EXPECT_TRUE(plan["is_active"].GetBool());
    }
    if (planId == draftPlanId) {
      sawDraft = true;
      EXPECT_FALSE(plan["is_active"].GetBool());
    }
  }
  EXPECT_TRUE(sawActive);
  EXPECT_TRUE(sawDraft);
}

TEST_F(WorkToolsTest, planGetReturnsFullEmbeddedChunkContent) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1");

  auto input = makeObject({{"plan_id", planId}});
  auto result = execute("plan_get", input);
  ASSERT_TRUE(result.success) << result.error;

  auto doc = parseJson(result.data);
  EXPECT_EQ(doc["id"].GetString(), std::string(planId));
  ASSERT_TRUE(doc.HasMember("chunks"));
  ASSERT_EQ(doc["chunks"].Size(), 1u);
  EXPECT_EQ(doc["chunks"][0]["id"].GetString(), std::string("chunk-1"));
  EXPECT_EQ(doc["chunks"][0]["goal"].GetString(), std::string("Goal"));
}

TEST_F(WorkToolsTest, planUpdatePerformsPartialUpdates) {
  const std::string planId = createPlanDirect();

  auto input = makeObject({{"plan_id", planId},
                           {"notes", "updated notes"},
                           {"status", "Paused"}});
  ToolResult result;
  const auto events = captureEvents([&]() { result = execute("plan_update", input); });
  ASSERT_TRUE(result.success) << result.error;

  const auto updated = threadManager_->getPlan(threadId_, planId);
  EXPECT_EQ(updated.notes, "updated notes");
  EXPECT_EQ(updated.status, PlanStatus::Paused);
  EXPECT_EQ(updated.title, "Plan A");
  EXPECT_EQ(updated.objective, "Ship tool APIs");

  const auto *event = findEvent<PlanUpdated>(events);
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(event->threadId, threadId_);
  EXPECT_EQ(event->plan.id, planId);
  EXPECT_EQ(event->plan.status, PlanStatus::Paused);
}

TEST_F(WorkToolsTest, planSetActiveUpdatesThreadMetadata) {
  const std::string firstPlanId = createPlanDirect("Plan 1");
  const std::string secondPlanId = createPlanDirect("Plan 2");
  auto metadata = threadManager_->getMetadata(threadId_);
  metadata.activePlanId = firstPlanId;
  threadManager_->updateMetadata(threadId_, metadata);

  auto input = makeObject({{"plan_id", secondPlanId}});
  ToolResult result;
  const auto events =
      captureEvents([&]() { result = execute("plan_set_active", input); });
  ASSERT_TRUE(result.success) << result.error;

  const auto updatedMetadata = threadManager_->getMetadata(threadId_);
  EXPECT_EQ(updatedMetadata.activePlanId, secondPlanId);
  EXPECT_EQ(threadManager_->getPlan(threadId_, secondPlanId).status,
            PlanStatus::Active);

  const auto *event = findEvent<PlanActivated>(events);
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(event->threadId, threadId_);
  EXPECT_EQ(event->planId, secondPlanId);
  EXPECT_EQ(event->plan.id, secondPlanId);
  EXPECT_EQ(event->plan.status, PlanStatus::Active);
}

TEST_F(WorkToolsTest, chunkAddBlocksDependencyIncompleteChunkInsteadOfMarkingReady) {
  const std::string planId = createPlanDirect();
  auto input = makeObject({{"plan_id", planId},
                           {"title", "Chunk title"},
                           {"goal", "Chunk goal"},
                           {"context", "Chunk context"},
                           {"constraints", "Chunk constraints"},
                           {"completion", "Chunk completion"}},
                          {},
                          {},
                          {{"depends_on", {"dep-1"}}});

  ToolResult result;
  const auto events = captureEvents([&]() { result = execute("chunk_add", input); });
  ASSERT_TRUE(result.success) << result.error;
  auto doc = parseJson(result.data);

  const auto plan = threadManager_->getPlan(threadId_, planId);
  ASSERT_EQ(plan.chunks.size(), 1u);
  EXPECT_EQ(plan.chunks[0].id, doc["chunk_id"].GetString());
  EXPECT_EQ(plan.chunks[0].status, WorkChunkStatus::Blocked);
  EXPECT_TRUE(plan.chunks[0].assignedAgentId.empty());
  ASSERT_EQ(plan.chunks[0].dependsOn.size(), 1u);
  EXPECT_EQ(plan.chunks[0].dependsOn[0], "dep-1");

  const auto *event = findEvent<ChunkAdded>(events);
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(event->threadId, threadId_);
  EXPECT_EQ(event->planId, planId);
  EXPECT_EQ(event->chunk.id, doc["chunk_id"].GetString());
  EXPECT_EQ(event->chunk.status, WorkChunkStatus::Blocked);
  EXPECT_TRUE(event->chunk.assignedAgentId.empty());
}

TEST_F(WorkToolsTest, chunkAddKeepsChunkReadyWhenDependenciesAreDone) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "dep-1", WorkChunkStatus::Done);

  auto input = makeObject({{"plan_id", planId},
                           {"title", "Chunk title"},
                           {"goal", "Chunk goal"},
                           {"context", "Chunk context"},
                           {"constraints", "Chunk constraints"},
                           {"completion", "Chunk completion"}},
                          {},
                          {},
                          {{"depends_on", {"dep-1"}}});

  auto result = execute("chunk_add", input);
  ASSERT_TRUE(result.success) << result.error;

  const auto plan = threadManager_->getPlan(threadId_, planId);
  ASSERT_EQ(plan.chunks.size(), 2u);
  EXPECT_EQ(plan.chunks[1].status, WorkChunkStatus::Ready);
}

TEST_F(WorkToolsTest, chunkAddPersistsPlanningGateFlag) {
  const std::string planId = createPlanDirect();
  auto input = makeObject({{"plan_id", planId},
                           {"title", "Design spec"},
                           {"goal", "Resolve planner design"},
                           {"context", "Lead doctrine"},
                           {"constraints", "No implementation yet"},
                           {"completion", "Reviewed design accepted"}},
                          {},
                          {{"planning_gate", true}});

  auto result = execute("chunk_add", input);
  ASSERT_TRUE(result.success) << result.error;

  const auto plan = threadManager_->getPlan(threadId_, planId);
  ASSERT_EQ(plan.chunks.size(), 1u);
  EXPECT_TRUE(plan.chunks[0].planningGate);
}

TEST_F(WorkToolsTest, chunkListReturnsSummaries) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1");

  auto input = makeObject({{"plan_id", planId}});
  auto result = execute("chunk_list", input);
  ASSERT_TRUE(result.success) << result.error;

  auto doc = parseJson(result.data);
  ASSERT_TRUE(doc.IsArray());
  ASSERT_EQ(doc.Size(), 1u);
  EXPECT_EQ(doc[0]["chunk_id"].GetString(), std::string("chunk-1"));
  EXPECT_EQ(doc[0]["title"].GetString(), std::string("Chunk chunk-1"));
  EXPECT_FALSE(doc[0].HasMember("goal"));
}

TEST_F(WorkToolsTest, chunkGetReturnsFullChunk) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1");

  auto input = makeObject({{"plan_id", planId}, {"chunk_id", "chunk-1"}});
  auto result = execute("chunk_get", input);
  ASSERT_TRUE(result.success) << result.error;

  auto doc = parseJson(result.data);
  EXPECT_EQ(doc["id"].GetString(), std::string("chunk-1"));
  EXPECT_EQ(doc["goal"].GetString(), std::string("Goal"));
  EXPECT_EQ(doc["completion"].GetString(), std::string("Completion"));
}

TEST_F(WorkToolsTest, chunkUpdatePerformsPartialUpdateAndRefreshesTimestamps) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1");
  const auto originalPlan = threadManager_->getPlan(threadId_, planId);
  const auto originalChunk = originalPlan.chunks[0];

  auto input = makeObject({{"plan_id", planId},
                           {"chunk_id", "chunk-1"},
                           {"status", "InProgress"},
                           {"result_summary", "started"}},
                          {{"attempt_count", 2}});
  ToolResult result;
  const auto events =
      captureEvents([&]() { result = execute("chunk_update", input); });
  ASSERT_TRUE(result.success) << result.error;

  const auto updatedPlan = threadManager_->getPlan(threadId_, planId);
  ASSERT_EQ(updatedPlan.chunks.size(), 1u);
  EXPECT_EQ(updatedPlan.chunks[0].status, WorkChunkStatus::InProgress);
  EXPECT_EQ(updatedPlan.chunks[0].attemptCount, 2);
  EXPECT_EQ(updatedPlan.chunks[0].resultSummary, "started");
  EXPECT_GE(updatedPlan.chunks[0].updatedAt, originalChunk.updatedAt);
  EXPECT_GE(updatedPlan.updatedAt, originalPlan.updatedAt);

  const auto *updatedEvent = findEvent<ChunkUpdated>(events);
  ASSERT_NE(updatedEvent, nullptr);
  EXPECT_EQ(updatedEvent->threadId, threadId_);
  EXPECT_EQ(updatedEvent->planId, planId);
  EXPECT_EQ(updatedEvent->chunk.id, "chunk-1");
  EXPECT_EQ(updatedEvent->chunk.status, WorkChunkStatus::InProgress);

  const auto *statusEvent = findEvent<ChunkStatusChanged>(events);
  ASSERT_NE(statusEvent, nullptr);
  EXPECT_EQ(statusEvent->threadId, threadId_);
  EXPECT_EQ(statusEvent->planId, planId);
  EXPECT_EQ(statusEvent->chunkId, "chunk-1");
  EXPECT_EQ(statusEvent->oldStatus, WorkChunkStatus::Ready);
  EXPECT_EQ(statusEvent->newStatus, WorkChunkStatus::InProgress);
  EXPECT_EQ(statusEvent->chunk.status, WorkChunkStatus::InProgress);
}

TEST_F(WorkToolsTest, chunkUpdateOmitsStatusChangeEventWhenStatusDoesNotChange) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::Ready);

  auto input = makeObject({{"plan_id", planId},
                           {"chunk_id", "chunk-1"},
                           {"result_summary", "still ready"}});
  ToolResult result;
  const auto events =
      captureEvents([&]() { result = execute("chunk_update", input); });
  ASSERT_TRUE(result.success) << result.error;

  EXPECT_EQ(countEvents<ChunkUpdated>(events), 1u);
  EXPECT_EQ(countEvents<ChunkStatusChanged>(events), 0u);
}

TEST_F(WorkToolsTest, chunkReadyForExecutionRespectsDependencyStatus) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "done-chunk", WorkChunkStatus::Done);
  addChunkDirect(planId, "ready-now", WorkChunkStatus::Ready, {"done-chunk"});
  addChunkDirect(planId, "blocked", WorkChunkStatus::Ready, {"missing"});
  addChunkDirect(planId, "not-ready", WorkChunkStatus::InProgress);

  auto input = makeObject({{"plan_id", planId}});
  auto result = execute("chunk_ready_for_execution", input);
  ASSERT_TRUE(result.success) << result.error;

  auto doc = parseJson(result.data);
  ASSERT_TRUE(doc.IsArray());
  ASSERT_EQ(doc.Size(), 1u);
  EXPECT_EQ(doc[0]["chunk_id"].GetString(), std::string("ready-now"));
}

TEST_F(WorkToolsTest, chunkReadyForExecutionSummaryIncludesPlanningGateFlag) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "design-gate", WorkChunkStatus::Ready);
  auto plan = threadManager_->getPlan(threadId_, planId);
  plan.chunks[0].planningGate = true;
  threadManager_->updatePlan(threadId_, plan);

  auto input = makeObject({{"plan_id", planId}});
  auto result = execute("chunk_ready_for_execution", input);
  ASSERT_TRUE(result.success) << result.error;

  auto doc = parseJson(result.data);
  ASSERT_TRUE(doc.IsArray());
  ASSERT_EQ(doc.Size(), 1u);
  EXPECT_TRUE(doc[0]["planning_gate"].GetBool());
}

TEST_F(WorkToolsTest, chunkUpdateCanMarkPlanningGateOnExistingChunk) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::Ready);

  auto input = makeObject({{"plan_id", planId}, {"chunk_id", "chunk-1"}},
                          {},
                          {{"planning_gate", true}});
  auto result = execute("chunk_update", input);
  ASSERT_TRUE(result.success) << result.error;

  const auto updatedPlan = threadManager_->getPlan(threadId_, planId);
  EXPECT_TRUE(updatedPlan.chunks[0].planningGate);
}

TEST_F(WorkToolsTest, chunkUpdateDowngradesReadyToBlockedWhenDependenciesAreNotDone) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "dep-1", WorkChunkStatus::InProgress);
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::InProgress, {"dep-1"});

  auto input = makeObject({{"plan_id", planId},
                           {"chunk_id", "chunk-1"},
                           {"status", "Ready"}});
  ToolResult result;
  const auto events =
      captureEvents([&]() { result = execute("chunk_update", input); });
  ASSERT_TRUE(result.success) << result.error;

  const auto updatedPlan = threadManager_->getPlan(threadId_, planId);
  EXPECT_EQ(updatedPlan.chunks[1].status, WorkChunkStatus::Blocked);

  const auto *statusEvent = findEvent<ChunkStatusChanged>(events);
  ASSERT_NE(statusEvent, nullptr);
  EXPECT_EQ(statusEvent->oldStatus, WorkChunkStatus::InProgress);
  EXPECT_EQ(statusEvent->newStatus, WorkChunkStatus::Blocked);
}

TEST_F(WorkToolsTest, chunkUpdateDowngradesReadyToBlockedWhenDependenciesChange) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::Ready);

  auto input = makeObject({{"plan_id", planId}, {"chunk_id", "chunk-1"}},
                          {},
                          {},
                          {{"depends_on", {"missing-dep"}}});
  auto result = execute("chunk_update", input);
  ASSERT_TRUE(result.success) << result.error;

  const auto updatedPlan = threadManager_->getPlan(threadId_, planId);
  EXPECT_EQ(updatedPlan.chunks[0].status, WorkChunkStatus::Blocked);
  ASSERT_EQ(updatedPlan.chunks[0].dependsOn.size(), 1u);
  EXPECT_EQ(updatedPlan.chunks[0].dependsOn[0], "missing-dep");
}

TEST_F(WorkToolsTest, leadMustRecordReviewSummaryBeforeMarkingChunkDone) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::Implemented);

  auto input = makeObject({{"plan_id", planId},
                           {"chunk_id", "chunk-1"},
                           {"status", "Done"}});
  auto result = execute("chunk_update", input);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error,
              ::testing::HasSubstr("cannot be marked Done without review_summary"));

  const auto updatedPlan = threadManager_->getPlan(threadId_, planId);
  EXPECT_EQ(updatedPlan.chunks[0].status, WorkChunkStatus::Implemented);
}

TEST_F(WorkToolsTest, leadCanAcceptChunkDoneWithReviewSummary) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::Implemented);

  auto input = makeObject({{"plan_id", planId},
                           {"chunk_id", "chunk-1"},
                           {"status", "Done"},
                           {"review_summary", "Reviewed file changes and verified focused regression coverage."}});
  auto result = execute("chunk_update", input);
  ASSERT_TRUE(result.success) << result.error;

  const auto updatedPlan = threadManager_->getPlan(threadId_, planId);
  EXPECT_EQ(updatedPlan.chunks[0].status, WorkChunkStatus::Done);
  EXPECT_EQ(updatedPlan.chunks[0].reviewSummary,
            "Reviewed file changes and verified focused regression coverage.");
}

TEST_F(WorkToolsTest, executorCanUpdateOnlyOwnChunkExecutionFields) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::Ready);

  auto plan = threadManager_->getPlan(threadId_, planId);
  plan.chunks[0].assignedAgentId = "executor-1";
  threadManager_->updatePlan(threadId_, plan);

  setAgentRole("executor",
               {ToolScope::Semantic, ToolScope::PlanRead, ToolScope::ChunkRead,
                ToolScope::ChunkWrite},
               "executor-1");

  auto input = makeObject({{"plan_id", planId},
                           {"chunk_id", "chunk-1"},
                           {"status", "Implemented"},
                           {"result_summary", "implementation complete"}},
                          {{"attempt_count", 3}});
  auto result = execute("chunk_update", input);
  ASSERT_TRUE(result.success) << result.error;

  const auto updatedPlan = threadManager_->getPlan(threadId_, planId);
  EXPECT_EQ(updatedPlan.chunks[0].status, WorkChunkStatus::Implemented);
  EXPECT_EQ(updatedPlan.chunks[0].attemptCount, 3);
  EXPECT_EQ(updatedPlan.chunks[0].resultSummary, "implementation complete");
}

TEST_F(WorkToolsTest, executorCannotMarkChunkDoneBeforeLeadReview) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::Implemented);

  auto plan = threadManager_->getPlan(threadId_, planId);
  plan.chunks[0].assignedAgentId = "executor-1";
  threadManager_->updatePlan(threadId_, plan);

  setAgentRole("executor",
               {ToolScope::Semantic, ToolScope::PlanRead, ToolScope::ChunkRead,
                ToolScope::ChunkWrite},
               "executor-1");

  auto input = makeObject({{"plan_id", planId},
                           {"chunk_id", "chunk-1"},
                           {"status", "Done"},
                           {"result_summary", "looks correct"}});
  auto result = execute("chunk_update", input);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error,
              ::testing::HasSubstr(
                  "Only the lead may mark a chunk Done after review"));
}

TEST_F(WorkToolsTest, executorCannotMutateAnotherChunk) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::Ready);
  addChunkDirect(planId, "chunk-2", WorkChunkStatus::Ready);

  auto plan = threadManager_->getPlan(threadId_, planId);
  plan.chunks[0].assignedAgentId = "executor-1";
  plan.chunks[1].assignedAgentId = "executor-2";
  threadManager_->updatePlan(threadId_, plan);

  setAgentRole("executor",
               {ToolScope::Semantic, ToolScope::PlanRead, ToolScope::ChunkRead,
                ToolScope::ChunkWrite},
               "executor-1");

  auto input = makeObject({{"plan_id", planId},
                           {"chunk_id", "chunk-2"},
                           {"status", "InProgress"}});
  auto result = execute("chunk_update", input);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error,
              ::testing::HasSubstr("executor may update only its assigned chunk"));
}

TEST_F(WorkToolsTest, executorCannotMutatePlanLevelFields) {
  const std::string planId = createPlanDirect();
  setAgentRole("executor", {ToolScope::PlanRead, ToolScope::ChunkRead,
                            ToolScope::ChunkWrite, ToolScope::Semantic},
               "executor-1");

  auto input = makeObject({{"plan_id", planId}, {"notes", "executor note"}});
  auto result = execute("plan_update", input);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error,
              ::testing::AnyOf(::testing::HasSubstr("Permission denied"),
                               ::testing::HasSubstr("lead agents")));
}

TEST_F(WorkToolsTest, executorCannotEditChunkDesignFields) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::Ready);

  auto plan = threadManager_->getPlan(threadId_, planId);
  plan.chunks[0].assignedAgentId = "executor-1";
  threadManager_->updatePlan(threadId_, plan);

  setAgentRole("executor",
               {ToolScope::Semantic, ToolScope::PlanRead, ToolScope::ChunkRead,
                ToolScope::ChunkWrite},
               "executor-1");

  auto input = makeObject({{"plan_id", planId},
                           {"chunk_id", "chunk-1"},
                           {"goal", "new goal"}});
  auto result = execute("chunk_update", input);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(
      result.error,
      ::testing::HasSubstr(
          "executor may update only status, attempt_count, and result_summary"));
}

TEST_F(WorkToolsTest, workerCannotMutatePlanOrChunkState) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1");
  setAgentRole("worker", {ToolScope::FilesystemRead, ToolScope::FilesystemWrite,
                          ToolScope::Process},
               "worker-1");

  auto planInput = makeObject({{"plan_id", planId}, {"notes", "worker note"}});
  auto planResult = execute("plan_update", planInput);
  EXPECT_FALSE(planResult.success);

  auto chunkInput = makeObject({{"plan_id", planId},
                                {"chunk_id", "chunk-1"},
                                {"status", "InProgress"}});
  auto chunkResult = execute("chunk_update", chunkInput);
  EXPECT_FALSE(chunkResult.success);
}

TEST_F(WorkToolsTest, scoutCannotMutatePlanOrChunkState) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1");
  setAgentRole("scout", {ToolScope::Semantic, ToolScope::PlanRead,
                         ToolScope::ChunkRead},
               "scout-1");

  auto planInput = makeObject({{"plan_id", planId}, {"notes", "scout note"}});
  auto planResult = execute("plan_update", planInput);
  EXPECT_FALSE(planResult.success);

  auto chunkInput = makeObject({{"plan_id", planId},
                                {"chunk_id", "chunk-1"},
                                {"status", "InProgress"}});
  auto chunkResult = execute("chunk_update", chunkInput);
  EXPECT_FALSE(chunkResult.success);
}

TEST_F(WorkToolsTest, auditorCanWriteReviewSummaryButNotExecutionFields) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1", WorkChunkStatus::Implemented);
  setAgentRole("auditor",
               {ToolScope::Semantic, ToolScope::PlanRead, ToolScope::ChunkRead,
                ToolScope::ChunkReview},
               "auditor-1");

  auto reviewInput = makeObject({{"plan_id", planId},
                                 {"chunk_id", "chunk-1"},
                                 {"review_summary", "Needs stronger regression coverage"}});
  auto reviewResult = execute("chunk_update", reviewInput);
  ASSERT_TRUE(reviewResult.success) << reviewResult.error;
  EXPECT_EQ(threadManager_->getPlan(threadId_, planId).chunks[0].reviewSummary,
            "Needs stronger regression coverage");

  auto statusInput = makeObject({{"plan_id", planId},
                                 {"chunk_id", "chunk-1"},
                                 {"status", "Done"}});
  auto statusResult = execute("chunk_update", statusInput);
  EXPECT_FALSE(statusResult.success);
  EXPECT_THAT(statusResult.error,
              ::testing::HasSubstr("auditor may update only review_summary"));
}

TEST_F(WorkToolsTest, workerCannotReadPlansWithoutReadScope) {
  const std::string planId = createPlanDirect();
  (void)planId;
  setAgentRole("worker", {ToolScope::FilesystemRead, ToolScope::Process},
               "worker-1");

  auto input = makeObject();
  auto result = execute("plan_list", input);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, ::testing::HasSubstr("Permission denied"));
}

TEST_F(WorkToolsTest, scoutCanReadButNotWriteChunkState) {
  const std::string planId = createPlanDirect();
  addChunkDirect(planId, "chunk-1");
  setAgentRole("scout", {ToolScope::Semantic, ToolScope::PlanRead,
                         ToolScope::ChunkRead},
               "scout-1");

  auto getInput = makeObject({{"plan_id", planId}, {"chunk_id", "chunk-1"}});
  auto getResult = execute("chunk_get", getInput);
  ASSERT_TRUE(getResult.success) << getResult.error;

  auto updateInput = makeObject({{"plan_id", planId},
                                 {"chunk_id", "chunk-1"},
                                 {"result_summary", "nope"}});
  auto updateResult = execute("chunk_update", updateInput);
  EXPECT_FALSE(updateResult.success);
}

} // namespace
