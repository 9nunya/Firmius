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
#include <atomic>
#include <future>
#include <thread>

#include <rapidjson/document.h>

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

    AgentTodoList createTestTodoList(const std::string& threadId,
                                     const std::string& agentId) {
        AgentTodoList todo;
        todo.threadId = threadId;
        todo.agentId = agentId;
        todo.nextId = 3;
        todo.items.push_back(TodoItem{1, "Inspect files", TodoStatus::InProgress,
                                      "chunk-1", "plan-1", 100, 150});
        todo.items.push_back(TodoItem{2, "Run tests", TodoStatus::Pending,
                                      "chunk-1", "plan-1", 110, 110});
        return todo;
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

TEST_F(ThreadManagerTest, writeAndReadAgentTodoRoundtrip) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);
    const std::string agentId = "agent-1";
    const auto original = createTestTodoList(threadId, agentId);

    tm->writeAgentTodo(threadId, agentId, original);
    const auto restored = tm->getAgentTodo(threadId, agentId);

    EXPECT_EQ(restored, original);
}

TEST_F(ThreadManagerTest, getAgentTodoMissingReturnsEmptyList) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    const auto todo = tm->getAgentTodo(threadId, "missing-agent");
    EXPECT_EQ(todo.threadId, threadId);
    EXPECT_EQ(todo.agentId, "missing-agent");
    EXPECT_EQ(todo.nextId, 1);
    EXPECT_TRUE(todo.items.empty());
}

TEST_F(ThreadManagerTest, mutateAgentTodoAppliesAtomicUpdate) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);
    const std::string agentId = "agent-1";
    tm->writeAgentTodo(threadId, agentId, createTestTodoList(threadId, agentId));

    auto mutated = tm->mutateAgentTodo(threadId, agentId, [&](AgentTodoList& todo) {
        todo.items[0].status = TodoStatus::Done;
        todo.items.push_back(TodoItem{3, "Finalize", TodoStatus::Pending, "", "", 200, 200});
        todo.nextId = 4;
    });

    EXPECT_EQ(mutated.nextId, 4);
    ASSERT_EQ(mutated.items.size(), 3u);
    EXPECT_EQ(mutated.items[0].status, TodoStatus::Done);
    EXPECT_EQ(mutated.items[2].id, 3);

    const auto reloaded = tm->getAgentTodo(threadId, agentId);
    EXPECT_EQ(reloaded, mutated);
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

TEST_F(ThreadManagerTest, listThreadsWithMetadata_skipsBrokenThreadDirectories) {
    ThreadMetadata good = createTestMetadata();
    good.title = "Healthy Thread";
    std::string goodId = tm->createThread(good);

    const std::string brokenId = "broken-thread";
    const auto brokenDir = std::filesystem::path(tempDir) / ".firmius" / "threads" / brokenId;
    std::filesystem::create_directories(brokenDir);
    {
        std::ofstream lockFile(brokenDir / ".lock");
        lockFile << "locked";
    }

    auto threads = tm->listThreadsWithMetadata();
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_EQ(threads.front().threadId, goodId);
    EXPECT_EQ(threads.front().title, "Healthy Thread");
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

TEST_F(ThreadManagerTest, updateMetadata_persistsRetryableRequest) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    auto loaded = tm->getMetadata(threadId);
    loaded.lastRetryableRequest = ThreadMetadata::RetryableRequest{
        "agent-7",
        "user-task-7",
        "retry me",
        {ImageContent{"data:image/png;base64,abc", "image/png", "auto"}},
        1234,
        true,
    };

    tm->updateMetadata(threadId, loaded);

    auto updated = tm->getMetadata(threadId);
    ASSERT_TRUE(updated.lastRetryableRequest.has_value());
    EXPECT_EQ(updated.lastRetryableRequest->targetAgentId, "agent-7");
    EXPECT_EQ(updated.lastRetryableRequest->turnId, "user-task-7");
    EXPECT_EQ(updated.lastRetryableRequest->text, "retry me");
    ASSERT_EQ(updated.lastRetryableRequest->images.size(), 1u);
    EXPECT_TRUE(updated.lastRetryableRequest->eligible);
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

TEST_F(ThreadManagerTest, mutatePlan_serializesConcurrentChunkAddsAcrossInstances) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);
    std::string planId = tm->createPlan(createTestPlan(threadId));
    const int addCount = 24;

    std::vector<std::future<void>> writers;
    for (int i = 0; i < addCount; ++i) {
        writers.push_back(std::async(std::launch::async, [&, i]() {
            ThreadManager localTm(tempDir + "/.firmius/threads");
            localTm.mutatePlan(threadId, planId, [&](Plan& plan) {
                WorkChunk chunk;
                chunk.id = "chunk-" + std::to_string(i + 2);
                chunk.title = "Chunk " + std::to_string(i);
                chunk.goal = "Goal";
                chunk.context = "Context";
                chunk.constraints = "Constraints";
                chunk.completion = "Completion";
                chunk.createdAt = static_cast<uint64_t>(i + 1);
                chunk.updatedAt = chunk.createdAt;
                plan.chunks.push_back(chunk);
            });
        }));
    }
    for (auto& writer : writers) {
        writer.get();
    }

    Plan loaded = tm->getPlan(threadId, planId);
    ASSERT_EQ(loaded.chunks.size(), static_cast<size_t>(addCount + 1));

    std::filesystem::path planPath =
        std::filesystem::path(tempDir) / ".firmius" / "threads" / threadId /
        "plans" / (planId + ".json");
    std::ifstream file(planPath);
    ASSERT_TRUE(file.is_open());
    std::string raw((std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>());
    rapidjson::Document doc;
    doc.Parse(raw.c_str());
    EXPECT_FALSE(doc.HasParseError());
    ASSERT_TRUE(doc.HasMember("chunks"));
    EXPECT_EQ(doc["chunks"].Size(), loaded.chunks.size());
}

TEST_F(ThreadManagerTest, mutatePlan_preventsLostUpdatesAndTornReadsDuringConcurrentAddAndUpdate) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);
    std::string planId = tm->createPlan(createTestPlan(threadId));
    std::filesystem::path planPath =
        std::filesystem::path(tempDir) / ".firmius" / "threads" / threadId /
        "plans" / (planId + ".json");

    constexpr int iterations = 40;
    std::atomic<bool> stopReader{false};
    std::atomic<int> parseFailures{0};

    std::thread reader([&]() {
        while (!stopReader.load()) {
            std::ifstream file(planPath);
            if (!file.is_open()) {
                continue;
            }
            std::string raw((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
            if (raw.empty()) {
                ++parseFailures;
                continue;
            }
            rapidjson::Document doc;
            doc.Parse(raw.c_str());
            if (doc.HasParseError() || !doc.IsObject()) {
                ++parseFailures;
            }
        }
    });

    auto addFuture = std::async(std::launch::async, [&]() {
        ThreadManager localTm(tempDir + "/.firmius/threads");
        for (int i = 0; i < iterations; ++i) {
            localTm.mutatePlan(threadId, planId, [&](Plan& plan) {
                WorkChunk chunk;
                chunk.id = "parallel-" + std::to_string(i);
                chunk.title = "Parallel " + std::to_string(i);
                chunk.goal = "Goal";
                chunk.context = "Context";
                chunk.constraints = "Constraints";
                chunk.completion = "Completion";
                chunk.createdAt = static_cast<uint64_t>(100 + i);
                chunk.updatedAt = chunk.createdAt;
                plan.chunks.push_back(chunk);
            });
        }
    });

    auto updateFuture = std::async(std::launch::async, [&]() {
        ThreadManager localTm(tempDir + "/.firmius/threads");
        for (int i = 0; i < iterations; ++i) {
            localTm.mutatePlan(threadId, planId, [&](Plan& plan) {
                auto& chunk = plan.chunks.front();
                chunk.attemptCount += 1;
                chunk.resultSummary = "attempt-" + std::to_string(chunk.attemptCount);
                chunk.updatedAt = static_cast<uint64_t>(200 + i);
            });
        }
    });

    addFuture.get();
    updateFuture.get();
    stopReader = true;
    reader.join();

    EXPECT_EQ(parseFailures.load(), 0);

    Plan loaded = tm->getPlan(threadId, planId);
    ASSERT_EQ(loaded.chunks.size(), static_cast<size_t>(iterations + 1));
    EXPECT_EQ(loaded.chunks.front().attemptCount, 1 + iterations);
    EXPECT_EQ(loaded.chunks.front().resultSummary,
              "attempt-" + std::to_string(1 + iterations));
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

TEST_F(ThreadManagerTest, artifactWriteReadListSupportsCreateAndUpdate) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    tm->writeAgentManifest(threadId, {
        {"agent-1", {"planner", "", "planner", "Planner", true}}
    });

    bool created = false;
    auto first = tm->writeArtifact(threadId, "agent-1", "planner",
                                   "REPORT.md", "v1", &created,
                                   std::optional<std::string>{"report"},
                                   std::optional<std::string>{"first"});
    EXPECT_TRUE(created);
    EXPECT_EQ(first.threadId, threadId);
    EXPECT_EQ(first.ownerAgentId, "agent-1");
    EXPECT_EQ(first.ownerFriendlyName, "planner");
    EXPECT_EQ(first.filename, "REPORT.md");
    EXPECT_EQ(first.storagePath, "artifacts/agent-1/REPORT.md");
    EXPECT_EQ(first.kind, std::optional<std::string>{"report"});
    EXPECT_EQ(first.description, std::optional<std::string>{"first"});

    const std::string firstRead = tm->readArtifact(threadId, "agent-1", "REPORT.md");
    EXPECT_EQ(firstRead, "v1");

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto second = tm->writeArtifact(threadId, "agent-1", "planner",
                                    "REPORT.md", "v2", &created);
    EXPECT_FALSE(created);
    EXPECT_EQ(second.createdAt, first.createdAt);
    EXPECT_GE(second.updatedAt, first.updatedAt);
    EXPECT_EQ(tm->readArtifact(threadId, "agent-1", "REPORT.md"), "v2");

    const auto artifacts = tm->listArtifacts(threadId);
    ASSERT_EQ(artifacts.size(), 1u);
    EXPECT_EQ(artifacts.front().filename, "REPORT.md");
    EXPECT_EQ(artifacts.front().ownerFriendlyName, "planner");
}

TEST_F(ThreadManagerTest, duplicateArtifactFilenamesAcrossAgentsDoNotCollide) {
    ThreadMetadata metadata = createTestMetadata();
    std::string threadId = tm->createThread(metadata);

    tm->writeAgentManifest(threadId, {
        {"agent-a", {"worker", "", "worker", "Worker", true}},
        {"agent-b", {"auditor", "", "auditor", "Auditor", true}}
    });

    bool createdA = false;
    bool createdB = false;
    tm->writeArtifact(threadId, "agent-a", "worker", "REPORT.md", "worker-body",
                      &createdA);
    tm->writeArtifact(threadId, "agent-b", "auditor", "REPORT.md", "auditor-body",
                      &createdB);
    EXPECT_TRUE(createdA);
    EXPECT_TRUE(createdB);

    EXPECT_EQ(tm->readArtifact(threadId, "agent-a", "REPORT.md"), "worker-body");
    EXPECT_EQ(tm->readArtifact(threadId, "agent-b", "REPORT.md"), "auditor-body");

    const auto artifacts = tm->listArtifacts(threadId);
    ASSERT_EQ(artifacts.size(), 2u);
    EXPECT_EQ(artifacts[0].filename, "REPORT.md");
    EXPECT_EQ(artifacts[1].filename, "REPORT.md");
    EXPECT_NE(artifacts[0].ownerAgentId, artifacts[1].ownerAgentId);
}

TEST_F(ThreadManagerTest, artifactsAreScopedByThread) {
    ThreadMetadata metadataA = createTestMetadata();
    metadataA.title = "Thread A";
    ThreadMetadata metadataB = createTestMetadata();
    metadataB.title = "Thread B";
    const std::string threadA = tm->createThread(metadataA);
    const std::string threadB = tm->createThread(metadataB);

    tm->writeAgentManifest(threadA, {
        {"agent-1", {"planner", "", "planner", "Planner", true}}
    });
    tm->writeAgentManifest(threadB, {
        {"agent-2", {"planner", "", "planner", "Planner", true}}
    });

    tm->writeArtifact(threadA, "agent-1", "planner", "A.md", "thread-a");
    tm->writeArtifact(threadB, "agent-2", "planner", "B.md", "thread-b");

    const auto artifactsA = tm->listArtifacts(threadA);
    const auto artifactsB = tm->listArtifacts(threadB);
    ASSERT_EQ(artifactsA.size(), 1u);
    ASSERT_EQ(artifactsB.size(), 1u);
    EXPECT_EQ(artifactsA.front().filename, "A.md");
    EXPECT_EQ(artifactsB.front().filename, "B.md");
}

}
