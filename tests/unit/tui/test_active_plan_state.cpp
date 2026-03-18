#include "ActivePlanState.hpp"
#include <gtest/gtest.h>

namespace {

using firmius::shared::ChunkAdded;
using firmius::shared::ChunkStatusChanged;
using firmius::shared::Plan;
using firmius::shared::PlanActivated;
using firmius::shared::ThreadMetadata;
using firmius::shared::WorkChunk;
using firmius::shared::WorkChunkStatus;
using firmius::tui::ActivePlanState;

Plan makePlan(const std::string &thread_id, const std::string &plan_id,
              const std::string &title) {
  Plan plan;
  plan.threadId = thread_id;
  plan.id = plan_id;
  plan.title = title;
  return plan;
}

WorkChunk makeChunk(const std::string &chunk_id, const std::string &title,
                    WorkChunkStatus status) {
  WorkChunk chunk;
  chunk.id = chunk_id;
  chunk.title = title;
  chunk.status = status;
  return chunk;
}

} // namespace

TEST(ActivePlanStateTest, HydratesCurrentThreadActivePlan) {
  ActivePlanState state;
  ThreadMetadata thread;
  thread.threadId = "thread-1";
  thread.activePlanId = "plan-1";

  auto plan = makePlan("thread-1", "plan-1", "Permissions Cleanup");
  plan.chunks.push_back(makeChunk("chunk-1", "Render lane", WorkChunkStatus::InProgress));
  state.hydrateForThread(thread, plan);

  ASSERT_TRUE(state.hasActivePlan());
  EXPECT_EQ(state.model().plan_id, "plan-1");
  EXPECT_EQ(state.model().plan_title, "Permissions Cleanup");
  EXPECT_EQ(state.model().chunks.size(), 1u);
  EXPECT_EQ(state.model().chunks.front().status_label, "In Progress");
}

TEST(ActivePlanStateTest, ClearsWhenThreadHasNoActivePlan) {
  ActivePlanState state;
  ThreadMetadata thread;
  thread.threadId = "thread-1";
  auto plan = makePlan("thread-1", "plan-1", "Permissions Cleanup");

  state.hydrateForThread(thread, plan);

  EXPECT_FALSE(state.hasActivePlan());
  EXPECT_FALSE(state.model().visible);
}

TEST(ActivePlanStateTest, PlanActivationAndChunkEventsUpdateModel) {
  ActivePlanState state;

  auto plan = makePlan("thread-1", "plan-1", "Permissions Cleanup");
  plan.chunks.push_back(makeChunk("chunk-1", "Wire events", WorkChunkStatus::Ready));

  EXPECT_TRUE(state.handleEvent(PlanActivated{"thread-1", "plan-1", plan},
                                "thread-1"));
  ASSERT_TRUE(state.hasActivePlan());
  EXPECT_EQ(state.model().collapsed_summary,
            "Plan: Permissions Cleanup | 1 waiting");

  WorkChunk updated_chunk =
      makeChunk("chunk-1", "Wire events", WorkChunkStatus::Verifying);
  EXPECT_TRUE(
      state.handleEvent(ChunkStatusChanged{"thread-1", "plan-1", "chunk-1",
                                           WorkChunkStatus::Ready,
                                           WorkChunkStatus::Verifying,
                                           updated_chunk},
                        "thread-1"));
  ASSERT_EQ(state.model().chunks.size(), 1u);
  EXPECT_EQ(state.model().chunks.front().status_label, "Verifying");
  EXPECT_EQ(state.model().collapsed_summary,
            "Plan: Permissions Cleanup | 1 verifying");

  WorkChunk new_chunk =
      makeChunk("chunk-2", "Render lane", WorkChunkStatus::InProgress);
  EXPECT_TRUE(state.handleEvent(
      ChunkAdded{"thread-1", "plan-1", new_chunk}, "thread-1"));
  EXPECT_EQ(state.model().chunks.size(), 2u);
  EXPECT_EQ(state.model().collapsed_summary,
            "Plan: Permissions Cleanup | 1 implementing | 1 verifying");
}

TEST(ActivePlanStateTest, IgnoresEventsForOtherThreads) {
  ActivePlanState state;
  auto plan = makePlan("thread-1", "plan-1", "Permissions Cleanup");

  EXPECT_FALSE(state.handleEvent(PlanActivated{"thread-2", "plan-1", plan},
                                 "thread-1"));
  EXPECT_FALSE(state.hasActivePlan());
}

TEST(ActivePlanStateTest, CollapsedSummaryBucketsRemainCompact) {
  auto plan = makePlan("thread-1", "plan-1", "Permissions Cleanup");
  plan.chunks = {
      makeChunk("draft", "Draft", WorkChunkStatus::Draft),
      makeChunk("ready", "Ready", WorkChunkStatus::Ready),
      makeChunk("progress", "Progress", WorkChunkStatus::InProgress),
      makeChunk("implemented", "Implemented", WorkChunkStatus::Implemented),
      makeChunk("verify", "Verify", WorkChunkStatus::Verifying),
      makeChunk("done", "Done", WorkChunkStatus::Done),
      makeChunk("blocked", "Blocked", WorkChunkStatus::Blocked),
      makeChunk("failed", "Failed", WorkChunkStatus::Failed),
  };

  EXPECT_EQ(ActivePlanState::collapsedSummary(plan),
            "Plan: Permissions Cleanup | 1 implementing | 1 implemented | 1 verifying | 2 waiting | 1 done | 1 blocked | 1 failed");
}
