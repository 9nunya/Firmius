#include <gtest/gtest.h>
#include "persistence/ThreadManager.hpp"
#include "persistence/Journaler.hpp"
#include "Serialization.hpp"
#include "Context.hpp"
#include "Message.hpp"
#include "Enums.hpp"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <algorithm>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

class ThreadManagerTest : public ::testing::Test {
protected:
    std::string tempDir;
    std::string originalHome;

    void SetUp() override {
        char tempTemplate[] = "/tmp/firmius_test_XXXXXX";
        char* result = mkdtemp(tempTemplate);
        ASSERT_NE(result, nullptr);
        tempDir = std::string(result);

        originalHome = getenv("HOME") ? std::string(getenv("HOME")) : "";
        setenv("HOME", tempDir.c_str(), 1);

        std::filesystem::create_directories(tempDir + "/.firmius/threads");
        tm = std::make_unique<ThreadManager>(tempDir + "/.firmius/threads");
    }

    std::unique_ptr<ThreadManager> tm;

    void TearDown() override {
        if (!originalHome.empty()) {
            setenv("HOME", originalHome.c_str(), 1);
        } else {
            unsetenv("HOME");
        }

        std::filesystem::remove_all(tempDir);
    }

    ThreadMetadata createTestMetadata() {
        ThreadMetadata metadata;
        metadata.title = "Test Thread";
        metadata.hostOptions.type = HostType::Local;
        metadata.hostIdentifier = "test-host";
        metadata.cwd = "/home/test";
        metadata.leadPersona = "test-coder";
        return metadata;
    }

    AgentTurn createTestTurn(const std::string& turnId) {
        AgentTurn turn;
        turn.turnId = turnId;
        turn.stopReason = StopReason::Stop;

        Message msg;
        msg.id = "msg-" + turnId;
        msg.role = Role::Assistant;
        msg.content.push_back(TextContent{"Test message"});
        msg.timestamp = 1234567890;
        turn.messages.push_back(msg);

        turn.metrics.tokens.prompt = 10;
        turn.metrics.tokens.completion = 5;
        turn.metrics.estimatedCostUsd = 0.001;

        return turn;
    }

    Plan createTestPlan(const std::string& threadId,
                        const std::string& planId = "") {
        Plan plan;
        plan.id = planId;
        plan.threadId = threadId;
        plan.title = "Work Language Migration";
        plan.objective = "Persist plans";
        plan.context = "Chunk 1";
        plan.strategy = "Shared models and thread storage";
        plan.status = PlanStatus::Draft;
        plan.notes = "No live events";

        WorkChunk chunk;
        chunk.id = "chunk-1";
        chunk.title = "Add models";
        chunk.goal = "Create plan types";
        chunk.context = "Shared contracts";
        chunk.constraints = "Chunk 1 only";
        chunk.completion = "Compile and persist";
        chunk.status = WorkChunkStatus::Ready;
        chunk.dependsOn = {"bootstrap"};
        chunk.assignedAgentId = "lead";
        chunk.attemptCount = 1;
        chunk.resultSummary = "In progress";
        chunk.reviewSummary = "";
        plan.chunks.push_back(chunk);

        return plan;
    }
};

TEST_F(ThreadManagerTest, createThread_roundtrip) {
    ThreadMetadata metadata = createTestMetadata();

    std::string threadId = tm->createThread(metadata);

    EXPECT_FALSE(threadId.empty());

    ThreadMetadata loaded = tm->getMetadata(threadId);

    EXPECT_EQ(loaded.title, metadata.title);
    EXPECT_EQ(loaded.hostOptions, metadata.hostOptions);
    EXPECT_EQ(loaded.hostIdentifier, metadata.hostIdentifier);
    EXPECT_EQ(loaded.cwd, metadata.cwd);
    EXPECT_EQ(loaded.leadPersona, metadata.leadPersona);
}

TEST_F(ThreadManagerTest, getMetadata_missingFile) {
    EXPECT_THROW({
        tm->getMetadata("nonexistent-thread-id");
    }, std::runtime_error);
}

TEST_F(ThreadManagerTest, getMetadata_corruptJson) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    std::string metadataPath = tempDir + "/.firmius/threads/" + threadId + "/metadata.json";
    std::ofstream file(metadataPath);
    file << "{invalid json content";
    file.close();

    EXPECT_THROW({
        tm->getMetadata(threadId);
    }, std::runtime_error);
}

TEST_F(ThreadManagerTest, listThreads_empty) {
    std::vector<std::string> threads = tm->listThreads();

    EXPECT_TRUE(threads.empty());
}

TEST_F(ThreadManagerTest, listThreads_multiple) {
    ThreadMetadata metadata1 = createTestMetadata();
    metadata1.title = "Thread 1";

    ThreadMetadata metadata2 = createTestMetadata();
    metadata2.title = "Thread 2";

    ThreadMetadata metadata3 = createTestMetadata();
    metadata3.title = "Thread 3";

    std::string id1 = tm->createThread(metadata1);
    std::string id2 = tm->createThread(metadata2);
    std::string id3 = tm->createThread(metadata3);

    std::vector<std::string> threads = tm->listThreads();

    EXPECT_EQ(threads.size(), 3u);

    EXPECT_TRUE(std::find(threads.begin(), threads.end(), id1) != threads.end());
    EXPECT_TRUE(std::find(threads.begin(), threads.end(), id2) != threads.end());
    EXPECT_TRUE(std::find(threads.begin(), threads.end(), id3) != threads.end());
}

TEST_F(ThreadManagerTest, loadAgentHistory_empty) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);
    std::string agentId = "test-agent";

    AgentHistory history = tm->loadAgentHistory(threadId, agentId);

    EXPECT_EQ(history.threadId, threadId);
    EXPECT_TRUE(history.turns.empty());
}

TEST_F(ThreadManagerTest, loadAgentHistory_withTurns) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);
    std::string agentId = "test-agent";

    {
        Journaler journaler(threadId, agentId);
        AgentTurn turn1 = createTestTurn("turn-001");
        AgentTurn turn2 = createTestTurn("turn-002");
        journaler.appendTurn(turn1);
        journaler.appendTurn(turn2);
    }

    AgentHistory history = tm->loadAgentHistory(threadId, agentId);

    EXPECT_EQ(history.threadId, threadId);
    EXPECT_EQ(history.turns.size(), 2u);
    EXPECT_EQ(history.turns[0].turnId, "turn-001");
    EXPECT_EQ(history.turns[1].turnId, "turn-002");
}

TEST_F(ThreadManagerTest, updateMetadata_persistsPermissionMode) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    auto loaded = tm->getMetadata(threadId);
    EXPECT_EQ(loaded.permissionMode, ThreadPermissionMode::Request);

    loaded.permissionMode = ThreadPermissionMode::AlwaysAllow;
    tm->updateMetadata(threadId, loaded);

    auto updated = tm->getMetadata(threadId);
    EXPECT_EQ(updated.permissionMode, ThreadPermissionMode::AlwaysAllow);
}

TEST_F(ThreadManagerTest, updateMetadata_persistsActivePlanId) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    auto loaded = tm->getMetadata(threadId);
    EXPECT_TRUE(loaded.activePlanId.empty());

    loaded.activePlanId = "plan-active";
    tm->updateMetadata(threadId, loaded);

    auto updated = tm->getMetadata(threadId);
    EXPECT_EQ(updated.activePlanId, "plan-active");
}

TEST_F(ThreadManagerTest, createAndGetPlan_roundtrip) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    Plan plan = createTestPlan(threadId);
    std::string planId = tm->createPlan(plan);

    EXPECT_FALSE(planId.empty());

    Plan loaded = tm->getPlan(threadId, planId);
    EXPECT_EQ(loaded.id, planId);
    EXPECT_EQ(loaded.threadId, threadId);
    EXPECT_EQ(loaded.title, "Work Language Migration");
    ASSERT_EQ(loaded.chunks.size(), 1u);
    EXPECT_EQ(loaded.chunks[0].id, "chunk-1");

    std::filesystem::path planPath =
        std::filesystem::path(tempDir) / ".firmius" / "threads" / threadId /
        "plans" / (planId + ".json");
    EXPECT_TRUE(std::filesystem::exists(planPath));
}

TEST_F(ThreadManagerTest, writePlan_createsPlansDirectory) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    Plan plan = createTestPlan(threadId, "plan-manual");
    tm->writePlan(threadId, plan);

    std::filesystem::path plansDir =
        std::filesystem::path(tempDir) / ".firmius" / "threads" / threadId /
        "plans";
    EXPECT_TRUE(std::filesystem::exists(plansDir));

    Plan loaded = tm->getPlan(threadId, "plan-manual");
    EXPECT_EQ(loaded.id, "plan-manual");
    EXPECT_EQ(loaded.threadId, threadId);
}

TEST_F(ThreadManagerTest, listPlans_returnsPersistedPlans) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    tm->writePlan(threadId, createTestPlan(threadId, "plan-b"));
    tm->writePlan(threadId, createTestPlan(threadId, "plan-a"));

    auto plans = tm->listPlans(threadId);
    ASSERT_EQ(plans.size(), 2u);
    EXPECT_EQ(plans[0].id, "plan-a");
    EXPECT_EQ(plans[1].id, "plan-b");
}

TEST_F(ThreadManagerTest, updatePlan_preservesCreatedAtAndRefreshesUpdatedAt) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    std::string planId = tm->createPlan(createTestPlan(threadId));
    Plan existing = tm->getPlan(threadId, planId);

    existing.status = PlanStatus::Paused;
    existing.notes = "Updated";
    uint64_t originalCreatedAt = existing.createdAt;
    uint64_t originalUpdatedAt = existing.updatedAt;

    tm->updatePlan(threadId, existing);

    Plan updated = tm->getPlan(threadId, planId);
    EXPECT_EQ(updated.createdAt, originalCreatedAt);
    EXPECT_GE(updated.updatedAt, originalUpdatedAt);
    EXPECT_EQ(updated.status, PlanStatus::Paused);
    EXPECT_EQ(updated.notes, "Updated");
}

TEST_F(ThreadManagerTest, getPlan_appliesBackwardCompatibleDefaults) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    std::filesystem::path planPath =
        std::filesystem::path(tempDir) / ".firmius" / "threads" / threadId /
        "plans" / "legacy-plan.json";
    std::filesystem::create_directories(planPath.parent_path());

    std::ofstream file(planPath);
    file << R"({"id":"legacy-plan","title":"Legacy plan"})";
    file.close();

    Plan loaded = tm->getPlan(threadId, "legacy-plan");
    EXPECT_EQ(loaded.id, "legacy-plan");
    EXPECT_EQ(loaded.threadId, threadId);
    EXPECT_EQ(loaded.title, "Legacy plan");
    EXPECT_EQ(loaded.status, PlanStatus::Draft);
    EXPECT_TRUE(loaded.chunks.empty());
}

TEST_F(ThreadManagerTest, permissionRules_roundtrip) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    ThreadPermissionRules rules;
    rules.commandAllowRules.push_back(
        {"git status", "git status", "git", CommandSeverity::LOW});
    rules.writeAllowPaths.push_back("/tmp/work/src/**");

    tm->writePermissionRules(threadId, rules);

    auto loaded = tm->readPermissionRules(threadId);
    ASSERT_EQ(loaded.commandAllowRules.size(), 1u);
    EXPECT_EQ(loaded.commandAllowRules[0].exactCommand, "git status");
    EXPECT_EQ(loaded.commandAllowRules[0].normalizedCommand, "git status");
    EXPECT_EQ(loaded.commandAllowRules[0].primaryCommand, "git");
    EXPECT_EQ(loaded.commandAllowRules[0].severity, CommandSeverity::LOW);
    ASSERT_EQ(loaded.writeAllowPaths.size(), 1u);
    EXPECT_EQ(loaded.writeAllowPaths[0], "/tmp/work/src/**");
}

TEST_F(ThreadManagerTest, addPermissionRules_deduplicatesEntries) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    CommandAllowRule commandRule{"git status", "git status", "git",
                                 CommandSeverity::LOW};
    tm->addCommandAllowRule(threadId, commandRule);
    tm->addCommandAllowRule(threadId, commandRule);
    tm->addWriteAllowPath(threadId, "/tmp/work/src/**");
    tm->addWriteAllowPath(threadId, "/tmp/work/src/**");

    auto loaded = tm->readPermissionRules(threadId);
    EXPECT_EQ(loaded.commandAllowRules.size(), 1u);
    EXPECT_EQ(loaded.writeAllowPaths.size(), 1u);
}

}
