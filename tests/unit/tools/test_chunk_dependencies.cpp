// Chunk Dependency Regression Tests
// ==================================
// Tests automatic dependency reconciliation when chunks are marked Done.
// Uses ThreadManager directly to set up test scenarios, then verifies
// that reconcileChunkDependencies() correctly unblocks dependent chunks.

#include "tools/WorkToolCommon.hpp"
#include "persistence/ThreadManager.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <cstdlib>
#include <chrono>

using namespace firmius::core;
using namespace firmius::shared;
using ::testing::_;

namespace {

std::string gTempDir;

void setupTempDir() {
  char tempTemplate[] = "/tmp/firmius_test_XXXXXX";
  char *result = mkdtemp(tempTemplate);
  ASSERT_NE(result, nullptr);
  gTempDir = result;
  std::filesystem::create_directories(gTempDir + "/.firmius/threads");
}

void cleanupTempDir() {
  if (!gTempDir.empty()) {
    std::filesystem::remove_all(gTempDir);
  }
}

std::string createTestThread(ThreadManager &tm) {
  ThreadMetadata metadata;
  metadata.title = "Dependency Test Thread";
  metadata.hostOptions.type = HostType::Local;
  metadata.cwd = gTempDir;
  metadata.leadPersona = "lead";
  return tm.createThread(metadata);
}

std::string createTestPlan(ThreadManager &tm, const std::string &threadId) {
  Plan plan;
  plan.threadId = threadId;
  plan.title = "Dependency Test Plan";
  plan.objective = "Test automatic dependency reconciliation";
  plan.strategy = "Add chunks with dependencies, mark them Done, verify unblocking";
  plan.context = "Testing context";
  plan.notes = "Test notes";
  plan.status = PlanStatus::Active;
  plan.createdAt = worktools::nowEpochMs();
  plan.updatedAt = plan.createdAt;
  return tm.createPlan(plan);
}

std::string addChunk(ThreadManager &tm, const std::string &threadId,
                     const std::string &planId, const std::string &title,
                     const std::vector<std::string> &dependsOn = {},
                     WorkChunkStatus status = WorkChunkStatus::Ready) {
  Plan plan = tm.getPlan(threadId, planId);
  
  WorkChunk chunk;
  chunk.id = "chunk-" + std::to_string(plan.chunks.size());
  chunk.title = title;
  chunk.goal = "Test chunk: " + title;
  chunk.context = "Testing dependency: " + title;
  chunk.completion = title + " completed";
  chunk.status = status;
  chunk.dependsOn = dependsOn;
  chunk.createdAt = worktools::nowEpochMs();
  chunk.updatedAt = chunk.createdAt;
  
  // Block chunk if dependencies are not met
  if (!dependsOn.empty() && status == WorkChunkStatus::Ready) {
    worktools::blockChunkIfDependenciesIncomplete(plan, chunk);
  }
  
  plan.chunks.push_back(chunk);
  tm.updatePlan(threadId, plan);
  return chunk.id;
}

void markChunkDone(ThreadManager &tm, const std::string &threadId,
                   const std::string &planId, const std::string &chunkId) {
  Plan plan = tm.getPlan(threadId, planId);
  for (auto &chunk : plan.chunks) {
    if (chunk.id == chunkId) {
      chunk.status = WorkChunkStatus::Done;
      chunk.updatedAt = worktools::nowEpochMs();
      break;
    }
  }
  tm.updatePlan(threadId, plan);
}

WorkChunkStatus getChunkStatus(const Plan &plan, const std::string &chunkId) {
  for (const auto &chunk : plan.chunks) {
    if (chunk.id == chunkId) {
      return chunk.status;
    }
  }
  return WorkChunkStatus::Blocked;
}

} // namespace

class ChunkDependencyTest : public ::testing::Test {
protected:
  std::string threadId_;
  std::string planId_;
  std::unique_ptr<ThreadManager> tm_;

  void SetUp() override {
    if (gTempDir.empty()) {
      setupTempDir();
    }
    tm_ = std::make_unique<ThreadManager>(gTempDir + "/.firmius/threads");
    threadId_ = createTestThread(*tm_);
    planId_ = createTestPlan(*tm_, threadId_);
  }

  void TearDown() override {
    cleanupTempDir();
  }

  Plan getPlan() {
    return tm_->getPlan(threadId_, planId_);
  }
};

// ============================================================================
// TEST: Diamond Dependency Pattern
// ============================================================================

TEST_F(ChunkDependencyTest, DiamondPatternUnblocksCorrectly) {
  // Create diamond structure: A -> B,C,D -> E
  std::string chunkA = addChunk(*tm_, threadId_, planId_, "Chunk A - Root", {});
  std::string chunkB = addChunk(*tm_, threadId_, planId_, "Chunk B", {chunkA});
  std::string chunkC = addChunk(*tm_, threadId_, planId_, "Chunk C", {chunkA});
  std::string chunkD = addChunk(*tm_, threadId_, planId_, "Chunk D", {chunkA});
  std::string chunkE = addChunk(*tm_, threadId_, planId_, "Chunk E", {chunkB, chunkC, chunkD});

  // Initial state: Only A should be Ready
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkA), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkB), WorkChunkStatus::Blocked);
    EXPECT_EQ(getChunkStatus(plan, chunkC), WorkChunkStatus::Blocked);
    EXPECT_EQ(getChunkStatus(plan, chunkD), WorkChunkStatus::Blocked);
    EXPECT_EQ(getChunkStatus(plan, chunkE), WorkChunkStatus::Blocked);
  }

  // Mark A as Done - should unblock B, C, D
  markChunkDone(*tm_, threadId_, planId_, chunkA);

  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkA), WorkChunkStatus::Done);
    EXPECT_EQ(getChunkStatus(plan, chunkB), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkC), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkD), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkE), WorkChunkStatus::Blocked);
  }

  // Mark B, C as Done - E should still be blocked (waiting for D)
  markChunkDone(*tm_, threadId_, planId_, chunkB);
  markChunkDone(*tm_, threadId_, planId_, chunkC);

  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkE), WorkChunkStatus::Blocked);
  }

  // Mark D as Done - should unblock E
  markChunkDone(*tm_, threadId_, planId_, chunkD);

  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkE), WorkChunkStatus::Ready);
  }
}

// ============================================================================
// TEST: Linear Chain
// ============================================================================

TEST_F(ChunkDependencyTest, LinearChainUnblocksSequentially) {
  std::string chunkA = addChunk(*tm_, threadId_, planId_, "Chunk A", {});
  std::string chunkB = addChunk(*tm_, threadId_, planId_, "Chunk B", {chunkA});
  std::string chunkC = addChunk(*tm_, threadId_, planId_, "Chunk C", {chunkB});
  std::string chunkD = addChunk(*tm_, threadId_, planId_, "Chunk D", {chunkC});
  std::string chunkE = addChunk(*tm_, threadId_, planId_, "Chunk E", {chunkD});

  // Initial: Only A ready
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkA), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkB), WorkChunkStatus::Blocked);
  }

  // Mark each Done in sequence and verify next unblocks
  markChunkDone(*tm_, threadId_, planId_, chunkA);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkB), WorkChunkStatus::Ready);
  }

  markChunkDone(*tm_, threadId_, planId_, chunkB);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkC), WorkChunkStatus::Ready);
  }

  markChunkDone(*tm_, threadId_, planId_, chunkC);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkD), WorkChunkStatus::Ready);
  }

  markChunkDone(*tm_, threadId_, planId_, chunkD);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkE), WorkChunkStatus::Ready);
  }
}

// ============================================================================
// TEST: Multiple Roots with Convergence
// ============================================================================

TEST_F(ChunkDependencyTest, MultipleRootsConverge) {
  std::string chunkA = addChunk(*tm_, threadId_, planId_, "Root A", {});
  std::string chunkB = addChunk(*tm_, threadId_, planId_, "Root B", {});
  std::string chunkC = addChunk(*tm_, threadId_, planId_, "Root C", {});
  std::string chunkD = addChunk(*tm_, threadId_, planId_, "Converge D", {chunkA, chunkB, chunkC});

  // Initial: A, B, C should be Ready; D blocked
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkA), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkB), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkC), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkD), WorkChunkStatus::Blocked);
  }

  // Mark A Done - D still blocked
  markChunkDone(*tm_, threadId_, planId_, chunkA);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkD), WorkChunkStatus::Blocked);
  }

  // Mark B Done - D still blocked
  markChunkDone(*tm_, threadId_, planId_, chunkB);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkD), WorkChunkStatus::Blocked);
  }

  // Mark C Done - D should unblock
  markChunkDone(*tm_, threadId_, planId_, chunkC);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkD), WorkChunkStatus::Ready);
  }
}

// ============================================================================
// TEST: Complex Dependency Graph (User Request)
// ============================================================================
// 7 chunks: A (root), B,C,D,E (depend on A), F (depends on B,C), G (depends on all)

TEST_F(ChunkDependencyTest, ComplexGraphFromUserRequest) {
  // Create the complex structure
  std::string chunkA = addChunk(*tm_, threadId_, planId_, "A - Root", {});
  std::string chunkB = addChunk(*tm_, threadId_, planId_, "B", {chunkA});
  std::string chunkC = addChunk(*tm_, threadId_, planId_, "C", {chunkA});
  std::string chunkD = addChunk(*tm_, threadId_, planId_, "D", {chunkA});
  std::string chunkE = addChunk(*tm_, threadId_, planId_, "E", {chunkA});
  std::string chunkF = addChunk(*tm_, threadId_, planId_, "F", {chunkB, chunkC});
  std::string chunkG = addChunk(*tm_, threadId_, planId_, "G", {chunkA, chunkB, chunkC, chunkD, chunkE});

  // Initial state verification
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkA), WorkChunkStatus::Ready);
    for (const auto &chunkId : {chunkB, chunkC, chunkD, chunkE, chunkF, chunkG}) {
      EXPECT_EQ(getChunkStatus(plan, chunkId), WorkChunkStatus::Blocked);
    }
  }

  // Step 1: Mark A Done - should unblock B, C, D, E (but not F or G)
  markChunkDone(*tm_, threadId_, planId_, chunkA);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkB), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkC), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkD), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkE), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkF), WorkChunkStatus::Blocked);
    EXPECT_EQ(getChunkStatus(plan, chunkG), WorkChunkStatus::Blocked);
  }

  // Step 2: Mark B and C Done - should unblock F (G still blocked, needs D, E)
  markChunkDone(*tm_, threadId_, planId_, chunkB);
  markChunkDone(*tm_, threadId_, planId_, chunkC);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkF), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(plan, chunkG), WorkChunkStatus::Blocked);
  }

  // Step 3: Mark D Done - G still blocked (needs E)
  markChunkDone(*tm_, threadId_, planId_, chunkD);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkG), WorkChunkStatus::Blocked);
  }

  // Step 4: Mark E Done - G should finally unblock
  markChunkDone(*tm_, threadId_, planId_, chunkE);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkG), WorkChunkStatus::Ready);
  }
}

// ============================================================================
// TEST: Plan Reload Preserves Reconciled State
// ============================================================================

TEST_F(ChunkDependencyTest, PlanReloadPreservesReconciledState) {
  std::string chunkA = addChunk(*tm_, threadId_, planId_, "A", {});
  std::string chunkB = addChunk(*tm_, threadId_, planId_, "B", {chunkA});
  std::string chunkC = addChunk(*tm_, threadId_, planId_, "C", {chunkB});

  // Mark A Done
  markChunkDone(*tm_, threadId_, planId_, chunkA);

  // Verify B is Ready
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkB), WorkChunkStatus::Ready);
  }

  // Simulate "reload" by getting plan again (this calls reconcileChunkDependencies)
  Plan reloadedPlan = tm_->getPlan(threadId_, planId_);
  {
    EXPECT_EQ(getChunkStatus(reloadedPlan, chunkA), WorkChunkStatus::Done);
    EXPECT_EQ(getChunkStatus(reloadedPlan, chunkB), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(reloadedPlan, chunkC), WorkChunkStatus::Blocked);
  }
}

// ============================================================================
// TEST: No False Unblocking
// ============================================================================

TEST_F(ChunkDependencyTest, NoFalseUnblocking) {
  std::string chunkA = addChunk(*tm_, threadId_, planId_, "A", {});
  std::string chunkB = addChunk(*tm_, threadId_, planId_, "B", {});
  std::string chunkC = addChunk(*tm_, threadId_, planId_, "C", {chunkA, chunkB});

  // Mark only A Done - C should remain blocked
  markChunkDone(*tm_, threadId_, planId_, chunkA);
  {
    Plan plan = getPlan();
    EXPECT_EQ(getChunkStatus(plan, chunkC), WorkChunkStatus::Blocked);
  }
}

// ============================================================================
// TEST: Reconcile Function Direct Test
// ============================================================================

TEST_F(ChunkDependencyTest, ReconcileChunkDependenciesDirectTest) {
  std::string chunkA = addChunk(*tm_, threadId_, planId_, "A", {});
  std::string chunkB = addChunk(*tm_, threadId_, planId_, "B", {chunkA});
  std::string chunkC = addChunk(*tm_, threadId_, planId_, "C", {chunkB});

  // Manually set A to Done in the plan
  {
    Plan plan = getPlan();
    for (auto &chunk : plan.chunks) {
      if (chunk.id == chunkA) {
        chunk.status = WorkChunkStatus::Done;
      }
    }
    tm_->updatePlan(threadId_, plan);
  }

  // Get plan again - should auto-reconcile
  Plan reconciledPlan = tm_->getPlan(threadId_, planId_);
  {
    EXPECT_EQ(getChunkStatus(reconciledPlan, chunkA), WorkChunkStatus::Done);
    EXPECT_EQ(getChunkStatus(reconciledPlan, chunkB), WorkChunkStatus::Ready);
    EXPECT_EQ(getChunkStatus(reconciledPlan, chunkC), WorkChunkStatus::Blocked);
  }
}
