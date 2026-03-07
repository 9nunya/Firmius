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

}
