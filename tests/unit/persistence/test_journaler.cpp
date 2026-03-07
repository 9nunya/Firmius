#include <gtest/gtest.h>
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"
#include "Serialization.hpp"
#include "Context.hpp"
#include "Message.hpp"
#include "Enums.hpp"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

class JournalerTest : public ::testing::Test {
protected:
    std::string tempDir;
    std::string originalHome;
    std::string threadId;
    std::string agentId;

    void SetUp() override {
        char tempTemplate[] = "/tmp/firmius_test_XXXXXX";
        char* result = mkdtemp(tempTemplate);
        ASSERT_NE(result, nullptr);
        tempDir = std::string(result);

        originalHome = getenv("HOME") ? std::string(getenv("HOME")) : "";
        setenv("HOME", tempDir.c_str(), 1);

        threadId = "test-thread-" + std::to_string(getpid());
        agentId = "test-agent";

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

    AgentTurn createTestTurn(const std::string& turnId) {
        AgentTurn turn;
        turn.turnId = turnId;
        turn.stopReason = StopReason::Stop;

        Message msg;
        msg.id = "msg-" + turnId;
        msg.role = Role::Assistant;
        msg.content.push_back(TextContent{"Test message for " + turnId});
        msg.timestamp = 1234567890;
        turn.messages.push_back(msg);

        turn.metrics.tokens.prompt = 10;
        turn.metrics.tokens.completion = 5;
        turn.metrics.estimatedCostUsd = 0.001;

        return turn;
    }
};

TEST_F(JournalerTest, appendTurn_roundtrip) {
    AgentTurn turn = createTestTurn("turn-001");

    {
        Journaler journaler(threadId, agentId);
        journaler.appendTurn(turn);
    }

    AgentHistory history = tm->loadAgentHistory(threadId, agentId);

    EXPECT_EQ(history.turns.size(), 1u);
    EXPECT_EQ(history.turns[0].turnId, turn.turnId);
}

TEST_F(JournalerTest, appendTurn_multiple) {
    AgentTurn turn1 = createTestTurn("turn-001");
    AgentTurn turn2 = createTestTurn("turn-002");
    AgentTurn turn3 = createTestTurn("turn-003");

    {
        Journaler journaler(threadId, agentId);
        journaler.appendTurn(turn1);
        journaler.appendTurn(turn2);
        journaler.appendTurn(turn3);
    }

    AgentHistory history = tm->loadAgentHistory(threadId, agentId);

    EXPECT_EQ(history.turns.size(), 3u);
    EXPECT_EQ(history.turns[0].turnId, "turn-001");
    EXPECT_EQ(history.turns[1].turnId, "turn-002");
    EXPECT_EQ(history.turns[2].turnId, "turn-003");
}

TEST_F(JournalerTest, appendTurn_concurrent) {
    const int numThreads = 4;
    const int turnsPerThread = 10;

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    bool start = false;

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([this, t, &mutex, &cv, &start]() {
            Journaler journaler(threadId, agentId);

            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&start]() { return start; });
            }

            for (int i = 0; i < turnsPerThread; ++i) {
                AgentTurn turn = createTestTurn("thread-" + std::to_string(t) + "-turn-" + std::to_string(i));
                journaler.appendTurn(turn);
            }
        });
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        start = true;
    }
    cv.notify_all();

    for (auto& t : threads) {
        t.join();
    }

    AgentHistory history = tm->loadAgentHistory(threadId, agentId);

    EXPECT_EQ(history.turns.size(), static_cast<size_t>(numThreads * turnsPerThread));
}

TEST_F(JournalerTest, constructor_createsDirectory) {
    std::string threadDir = tempDir + "/.firmius/threads/" + threadId;
    std::string agentFile = threadDir + "/" + agentId + ".jsonl";

    EXPECT_FALSE(std::filesystem::exists(threadDir));

    {
        Journaler journaler(threadId, agentId);
        AgentTurn turn = createTestTurn("turn-001");
        journaler.appendTurn(turn);
    }

    EXPECT_TRUE(std::filesystem::exists(threadDir));
    EXPECT_TRUE(std::filesystem::exists(agentFile));
}

TEST_F(JournalerTest, destructor_closesFile) {
    std::string agentFile = tempDir + "/.firmius/threads/" + threadId + "/" + agentId + ".jsonl";

    {
        Journaler journaler(threadId, agentId);
        AgentTurn turn = createTestTurn("turn-001");
        journaler.appendTurn(turn);

        EXPECT_TRUE(std::filesystem::exists(agentFile));
    }

    std::ifstream file(agentFile);
    ASSERT_TRUE(file.is_open());

    std::string line;
    std::getline(file, line);
    EXPECT_FALSE(line.empty());

    file.close();
}

}
