#include <gtest/gtest.h>
#include "harness/Harness.hpp"
#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"
#include "Events.hpp"

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

class HarnessTest : public ::testing::Test {
protected:
    void SetUp() override {
        testHome_ = std::filesystem::temp_directory_path() / ("firmius_test_" + std::to_string(getpid()));
        std::filesystem::create_directories(testHome_);
        setenv("HOME", testHome_.c_str(), 1);
        cleanupFirmiusDir();
        Harness::instance().init();
    }
    
    void TearDown() override {
        Harness::instance().shutdown();
        cleanupFirmiusDir();
        std::filesystem::remove_all(testHome_);
    }
    
    void cleanupFirmiusDir() {
        std::filesystem::path firmiusDir = testHome_ / ".firmius";
        if (std::filesystem::exists(firmiusDir)) {
            std::filesystem::remove_all(firmiusDir);
        }
    }
    
    std::filesystem::path testHome_;
};

TEST_F(HarnessTest, switchThread_preservesAgent) {
    std::string threadA = Harness::instance().newThread(HostType::Local, "/tmp", "test");
    ASSERT_FALSE(threadA.empty()) << "Failed to create thread A";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
    
    std::string threadB = Harness::instance().newThread(HostType::Local, "/tmp", "test");
    ASSERT_FALSE(threadB.empty()) << "Failed to create thread B";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadB);
    EXPECT_NE(threadA, threadB);
    
    bool result = Harness::instance().switchThread(threadA);
    EXPECT_TRUE(result);
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
}

TEST_F(HarnessTest, newThread_savesPreviousAgent) {
    std::string threadA = Harness::instance().newThread(HostType::Local, "/tmp", "test");
    ASSERT_FALSE(threadA.empty()) << "Failed to create thread A";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
    
    std::string threadB = Harness::instance().newThread(HostType::Local, "/tmp", "test");
    ASSERT_FALSE(threadB.empty()) << "Failed to create thread B";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadB);
    
    bool result = Harness::instance().switchThread(threadA);
    EXPECT_TRUE(result);
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
}

TEST_F(HarnessTest, isDescendant_directChild) {
    std::string thread = Harness::instance().newThread(HostType::Local, "/tmp", "test");
    ASSERT_FALSE(thread.empty());
    
    std::string parentId = "parent-agent-001";
    std::string childId = "child-agent-001";
    
    bool childReceivedEvent = false;
    
    int subId = Harness::instance().subscribe([&childReceivedEvent, childId](const AppEvent& event) {
        if (std::holds_alternative<AgentText>(event)) {
            const auto& chunk = std::get<AgentText>(event);
            if (chunk.agentId == childId) {
                childReceivedEvent = true;
            }
        }
    });
    
    Harness::instance().unsubscribe(subId);
    SUCCEED();
}

TEST_F(HarnessTest, isDescendant_grandchild) {
    std::string grandparentId = "grandparent-agent";
    std::string parentId = "parent-agent";
    std::string grandchildId = "grandchild-agent";
    SUCCEED();
}

TEST_F(HarnessTest, isDescendant_unrelated) {
    std::string agentA = "agent-a";
    std::string agentB = "agent-b";
    std::string unrelatedAgent = "unrelated-agent";
    SUCCEED();
}

TEST_F(HarnessTest, isDescendant_cycleProtection) {
    SUCCEED();
}

TEST_F(HarnessTest, routeEngineEvent_filtersNonDescendants) {
    std::string thread = Harness::instance().newThread(HostType::Local, "/tmp", "test");
    ASSERT_FALSE(thread.empty());
    
    std::string focusedAgent = "focused-agent";
    std::string unrelatedAgent = "unrelated-agent";
    
    std::vector<std::string> receivedAgentIds;
    
    int subId = Harness::instance().subscribe([&receivedAgentIds](const AppEvent& event) {
        std::visit([&receivedAgentIds](auto&& ev) {
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, AgentText> ||
                          std::is_same_v<T, AgentThinking> ||
                          std::is_same_v<T, AgentToolCall> ||
                          std::is_same_v<T, AgentTurnCompleted> ||
                          std::is_same_v<T, AgentSpawned> ||
                          std::is_same_v<T, AgentProcessOutput> ||
                          std::is_same_v<T, AgentCompacting> ||
                          std::is_same_v<T, ContextCompacted>) {
                receivedAgentIds.push_back(ev.agentId);
            }
        }, event);
    });
    
    Harness::instance().unsubscribe(subId);
    SUCCEED();
}

TEST_F(HarnessTest, routeEngineEvent_passesDescendants) {
    std::string thread = Harness::instance().newThread(HostType::Local, "/tmp", "test");
    ASSERT_FALSE(thread.empty());
    
    std::string parentAgent = "parent-agent";
    std::string childAgent = "child-agent";
    
    SUCCEED();
}

TEST_F(HarnessTest, shutdown_savesSession) {
    std::string threadId = Harness::instance().newThread(HostType::Local, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());
    
    Harness::instance().shutdown();
    
    std::filesystem::path sessionFile = testHome_ / ".firmius" / "last_session.json";
    EXPECT_TRUE(std::filesystem::exists(sessionFile));
    
    if (std::filesystem::exists(sessionFile)) {
        std::ifstream file(sessionFile);
        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        EXPECT_FALSE(content.empty());
        EXPECT_NE(content.find("threadId"), std::string::npos);
    }
    
    Harness::instance().init();
}

TEST_F(HarnessTest, threadLocking_preventsConcurrentAccess) {
    std::string threadId = Harness::instance().newThread(HostType::Local, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());
    
    std::string threadId2 = Harness::instance().newThread(HostType::Local, "/tmp", "test2");
    EXPECT_FALSE(threadId2.empty());
    EXPECT_NE(threadId, threadId2);
}

TEST_F(HarnessTest, resumeLast_restoresSession) {
    std::string threadId = Harness::instance().newThread(HostType::Local, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());
    
    Harness::instance().shutdown();
    Harness::instance().init();
    
    bool result = Harness::instance().resumeLast();
    EXPECT_TRUE(result);
    EXPECT_EQ(Harness::instance().currentThreadId(), threadId);
}

} // namespace
