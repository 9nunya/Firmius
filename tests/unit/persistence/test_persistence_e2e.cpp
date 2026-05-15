#include <gtest/gtest.h>

#include "persistence/ThreadManager.hpp"
#include "persistence/Journaler.hpp"
#include "Serialization.hpp"
#include "Context.hpp"
#include "Message.hpp"
#include "Enums.hpp"

#include <filesystem>
#include <cstdlib>
#include <future>
#include <sqlite3.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

std::string jsonString(const rapidjson::Document& d) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    return buffer.GetString();
}

class PersistenceE2ETest : public ::testing::Test {
protected:
    std::string tempDir;
    std::string originalHome;
    std::unique_ptr<ThreadManager> tm;

    void SetUp() override {
        char tempTemplate[] = "/tmp/firmius_persistence_e2e_XXXXXX";
        char* result = mkdtemp(tempTemplate);
        ASSERT_NE(result, nullptr);
        tempDir = result;

        originalHome = getenv("HOME") ? std::string(getenv("HOME")) : "";
        setenv("HOME", tempDir.c_str(), 1);
        std::filesystem::create_directories(tempDir + "/.firmius/threads");
    }

    void TearDown() override {
        if (!originalHome.empty()) setenv("HOME", originalHome.c_str(), 1);
        else unsetenv("HOME");
        std::filesystem::remove_all(tempDir);
    }

    ThreadMetadata makeMetadata() {
        ThreadMetadata metadata;
        metadata.title = "E2E Thread";
        metadata.hostOptions.type = HostType::Local;
        metadata.hostIdentifier = "host-e2e";
        metadata.cwd = "/tmp/e2e";
        metadata.leadPersona = "lead";
        return metadata;
    }

    AgentTurn makeTurn(const std::string& id) {
        AgentTurn turn;
        turn.turnId = id;
        turn.stopReason = StopReason::Stop;
        Message msg;
        msg.id = "msg-" + id;
        msg.role = Role::Assistant;
        msg.timestamp = 100;
        msg.content.push_back(TextContent{"hello-" + id});
        turn.messages.push_back(msg);
        turn.metrics.tokens.prompt = 10;
        turn.metrics.tokens.completion = 5;
        turn.metrics.estimatedCostUsd = 0.01;
        return turn;
    }

    Plan makePlan(const std::string& threadId) {
        Plan plan;
        plan.id = "plan-e2e";
        plan.threadId = threadId;
        plan.title = "Persistence rewrite";
        plan.objective = "prove migration";
        plan.context = "core";
        plan.strategy = "normalize";
        plan.status = PlanStatus::Active;
        plan.notes = "active";

        WorkChunk chunk;
        chunk.id = "chunk-e2e";
        chunk.title = "normalize tables";
        chunk.goal = "ship v2";
        chunk.context = "persistence";
        chunk.constraints = "none";
        chunk.completion = "green";
        chunk.status = WorkChunkStatus::InProgress;
        chunk.dependsOn = {"bootstrap"};
        chunk.filesToRead = {"packages/core/src/persistence/ThreadManager.cpp"};
        chunk.filesToTouch = {"packages/core/src/persistence/ThreadManager.cpp"};
        chunk.cwd = "/mnt/SHIT/Projects/Firmius";
        chunk.verificationCondition = "tests pass";
        chunk.handoffNotes = "finish migration";

        WorkTask task;
        task.id = "task-e2e";
        task.title = "migrate";
        task.goal = "move blob rows";
        task.status = WorkChunkStatus::Ready;
        task.notes = "careful";
        task.verificationCondition = "data readable";
        task.assignedWorkerId = "coder";
        chunk.tasks.push_back(task);

        plan.chunks.push_back(chunk);
        return plan;
    }

    AgentTodoList makeTodo(const std::string& threadId, const std::string& agentId) {
        AgentTodoList todo;
        todo.threadId = threadId;
        todo.agentId = agentId;
        todo.nextId = 3;
        todo.items.push_back(TodoItem{1, "inspect", TodoStatus::InProgress, "chunk-e2e", "plan-e2e", 1, 2});
        todo.items.push_back(TodoItem{2, "verify", TodoStatus::Pending, "chunk-e2e", "plan-e2e", 2, 2});
        return todo;
    }
};

TEST_F(PersistenceE2ETest, migratesLegacyRowsAndUsesNormalizedReads) {
    const auto dbPath = std::filesystem::path(tempDir) / ".firmius" / "threads" / "firmius_threads.db";
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), SQLITE_OK);
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db,
                           "CREATE TABLE IF NOT EXISTS threads (thread_id TEXT PRIMARY KEY, metadata_json TEXT NOT NULL, created_at INTEGER NOT NULL, last_active_at INTEGER NOT NULL);"
                           "CREATE TABLE IF NOT EXISTS plans (thread_id TEXT NOT NULL, plan_id TEXT NOT NULL, plan_json TEXT NOT NULL, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, PRIMARY KEY(thread_id, plan_id));"
                           "CREATE TABLE IF NOT EXISTS agent_todos (thread_id TEXT NOT NULL, agent_id TEXT NOT NULL, todo_json TEXT NOT NULL, PRIMARY KEY(thread_id, agent_id));"
                           "CREATE TABLE IF NOT EXISTS agent_turns (id INTEGER PRIMARY KEY AUTOINCREMENT, thread_id TEXT NOT NULL, agent_id TEXT NOT NULL, turn_json TEXT NOT NULL);",
                           nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
    if (err) sqlite3_free(err);

    auto metadata = makeMetadata();
    metadata.threadId = "legacy-e2e-thread";
    metadata.createdAt = 10;
    metadata.lastActiveAt = 11;
    const auto plan = makePlan(metadata.threadId);
    const auto todo = makeTodo(metadata.threadId, "legacy-agent");
    const auto turn = makeTurn("legacy-turn");

    auto execLiteral = [&](const std::string& sql) {
        char* localErr = nullptr;
        ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &localErr), SQLITE_OK)
            << (localErr ? localErr : "");
        if (localErr) sqlite3_free(localErr);
    };

    execLiteral("INSERT INTO threads(thread_id, metadata_json, created_at, last_active_at) VALUES('legacy-e2e-thread', '" + serializeToString(AgentContext{}) + "', 10, 11);");
    execLiteral("UPDATE threads SET metadata_json='" + jsonString(toJson(metadata)) + "' WHERE thread_id='legacy-e2e-thread';");
    execLiteral("INSERT INTO plans(thread_id, plan_id, plan_json, created_at, updated_at) VALUES('legacy-e2e-thread', 'plan-e2e', '" + jsonString(toJson(plan)) + "', 10, 11);");
    execLiteral("INSERT INTO agent_todos(thread_id, agent_id, todo_json) VALUES('legacy-e2e-thread', 'legacy-agent', '" + jsonString(toJson(todo)) + "');");
    execLiteral("INSERT INTO agent_turns(thread_id, agent_id, turn_json) VALUES('legacy-e2e-thread', 'legacy-agent', '" + jsonString(toJson(turn)) + "');");
    sqlite3_close(db);

    tm = std::make_unique<ThreadManager>(tempDir + "/.firmius/threads");
    auto loadedMeta = tm->getMetadata("legacy-e2e-thread");
    auto loadedPlan = tm->getPlan("legacy-e2e-thread", "plan-e2e");
    auto loadedTodo = tm->getAgentTodo("legacy-e2e-thread", "legacy-agent");
    auto loadedHistory = tm->loadAgentHistory("legacy-e2e-thread", "legacy-agent");

    EXPECT_EQ(loadedMeta.title, metadata.title);
    EXPECT_EQ(loadedPlan.title, plan.title);
    ASSERT_EQ(loadedPlan.chunks.size(), 1u);
    ASSERT_EQ(loadedPlan.chunks[0].tasks.size(), 1u);
    EXPECT_EQ(loadedTodo.items.size(), todo.items.size());
    ASSERT_EQ(loadedHistory.turns.size(), 1u);
    EXPECT_EQ(loadedHistory.turns[0].turnId, turn.turnId);
}

TEST_F(PersistenceE2ETest, survivesConcurrentNormalizedChurn) {
    ThreadManager seeded(tempDir + "/.firmius/threads");
    tm = std::make_unique<ThreadManager>(tempDir + "/.firmius/threads");
    const std::string threadId = tm->createThread(makeMetadata());
    const std::string agentId = "e2e-agent";
    tm->writePlan(threadId, makePlan(threadId));
    tm->writeAgentTodo(threadId, agentId, makeTodo(threadId, agentId));

    constexpr int writers = 4;
    constexpr int iterations = 20;
    std::vector<std::future<void>> jobs;
    for (int i = 0; i < writers; ++i) {
        jobs.push_back(std::async(std::launch::async, [&, i]() {
            ThreadManager localTm(tempDir + "/.firmius/threads");
            Journaler journaler(threadId, agentId);
            for (int n = 0; n < iterations; ++n) {
                localTm.mutatePlan(threadId, "plan-e2e", [&](Plan& plan) {
                    plan.notes = "writer-" + std::to_string(i) + "-" + std::to_string(n);
                });
                localTm.mutateAgentTodo(threadId, agentId, [&](AgentTodoList& todo) {
                    todo.nextId = std::max(todo.nextId, 10 + n);
                });
                journaler.appendTurn(makeTurn("t-" + std::to_string(i) + "-" + std::to_string(n)));
            }
        }));
    }
    for (auto& job : jobs) job.get();

    const auto plan = tm->getPlan(threadId, "plan-e2e");
    const auto todo = tm->getAgentTodo(threadId, agentId);
    const auto history = tm->loadAgentHistory(threadId, agentId);

    EXPECT_FALSE(plan.notes.empty());
    EXPECT_GE(todo.nextId, 10);
    EXPECT_EQ(history.turns.size(), static_cast<size_t>(writers * iterations));
}

}
