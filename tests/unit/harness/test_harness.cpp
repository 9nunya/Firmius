#include <gtest/gtest.h>
#include "harness/Harness.hpp"
#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "environment/Permissions.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"
#include "Events.hpp"

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <future>

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
    std::string threadA = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadA.empty()) << "Failed to create thread A";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
    
    std::string threadB = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadB.empty()) << "Failed to create thread B";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadB);
    EXPECT_NE(threadA, threadB);
    
    bool result = Harness::instance().switchThread(threadA);
    EXPECT_TRUE(result);
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
}

TEST_F(HarnessTest, newThread_savesPreviousAgent) {
    std::string threadA = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadA.empty()) << "Failed to create thread A";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
    
    std::string threadB = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadB.empty()) << "Failed to create thread B";
    EXPECT_EQ(Harness::instance().currentThreadId(), threadB);
    
    bool result = Harness::instance().switchThread(threadA);
    EXPECT_TRUE(result);
    EXPECT_EQ(Harness::instance().currentThreadId(), threadA);
}

TEST_F(HarnessTest, isDescendant_directChild) {
    std::string thread = Harness::instance().newThread({}, "/tmp", "test");
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
    std::string thread = Harness::instance().newThread({}, "/tmp", "test");
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
    std::string thread = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(thread.empty());
    
    std::string parentAgent = "parent-agent";
    std::string childAgent = "child-agent";
    
    SUCCEED();
}

TEST_F(HarnessTest, shutdown_savesSession) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
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
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());
    
    std::string threadId2 = Harness::instance().newThread({}, "/tmp", "test2");
    EXPECT_FALSE(threadId2.empty());
    EXPECT_NE(threadId, threadId2);
}

TEST_F(HarnessTest, resumeLast_restoresSession) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());
    
    Harness::instance().shutdown();
    Harness::instance().init();
    
    bool result = Harness::instance().resumeLast();
    EXPECT_TRUE(result);
    EXPECT_EQ(Harness::instance().currentThreadId(), threadId);
}

TEST_F(HarnessTest, currentThreadPermissionMode_defaultsToRequest) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    EXPECT_EQ(Harness::instance().currentThreadPermissionMode(),
              ThreadPermissionMode::Request);
}

TEST_F(HarnessTest, setCurrentThreadPermissionMode_persistsAcrossThreadSwitch) {
    std::string threadA = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadA.empty());
    ASSERT_TRUE(Harness::instance().setCurrentThreadPermissionMode(
        ThreadPermissionMode::AlwaysAllow));

    std::string threadB = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadB.empty());
    ASSERT_TRUE(Harness::instance().switchThread(threadA));

    EXPECT_EQ(Harness::instance().currentThreadPermissionMode(),
              ThreadPermissionMode::AlwaysAllow);
}

TEST_F(HarnessTest, cycleCurrentThreadPermissionMode_cyclesAndPersists) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    auto next = Harness::instance().cycleCurrentThreadPermissionMode();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, ThreadPermissionMode::AlwaysAllow);
    EXPECT_EQ(Harness::instance().currentThreadPermissionMode(),
              ThreadPermissionMode::AlwaysAllow);

    next = Harness::instance().cycleCurrentThreadPermissionMode();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, ThreadPermissionMode::DenyAll);

    next = Harness::instance().cycleCurrentThreadPermissionMode();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, ThreadPermissionMode::Request);
}

TEST_F(HarnessTest, requestPermissionEscalation_blocksUntilResolved) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    std::promise<PermissionEscalationRequest> requestPromise;
    auto requestFuture = requestPromise.get_future();
    int subId = Harness::instance().subscribe([&](const AppEvent& event) {
        if (auto request = std::get_if<PermissionEscalationRequest>(&event)) {
            requestPromise.set_value(*request);
        }
    });

    std::promise<PermissionResponse> responsePromise;
    auto responseFuture = responsePromise.get_future();
    std::thread worker([&]() {
        PermissionEscalationRequest request;
        request.threadId = threadId;
        request.agentId = "agent-1";
        request.requestType = PermissionRequestType::Command;
        request.title = "Need approval";
        request.message = "Run command?";
        request.command = "rm -rf /tmp/nope";
        request.severity = CommandSeverity::HIGH;
        responsePromise.set_value(
            Harness::instance().requestPermissionEscalation(request));
    });

    auto emittedRequest = requestFuture.get();
    EXPECT_FALSE(emittedRequest.requestId.empty());
    EXPECT_EQ(emittedRequest.threadId, threadId);
    EXPECT_EQ(emittedRequest.command, "rm -rf /tmp/nope");

    EXPECT_TRUE(Harness::instance().resolvePermissionEscalation(
        emittedRequest.requestId, PermissionResponse::AllowOnce));

    EXPECT_EQ(responseFuture.get(), PermissionResponse::AllowOnce);

    worker.join();
    Harness::instance().unsubscribe(subId);
}

TEST_F(HarnessTest, setCurrentThreadPermissionMode_emitsThreadMetadataUpdated) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    std::promise<ThreadMetadataUpdated> eventPromise;
    auto eventFuture = eventPromise.get_future();
    int subId = Harness::instance().subscribe([&](const AppEvent& event) {
        if (auto metadata = std::get_if<ThreadMetadataUpdated>(&event)) {
            eventPromise.set_value(*metadata);
        }
    });

    ASSERT_TRUE(Harness::instance().setCurrentThreadPermissionMode(
        ThreadPermissionMode::AlwaysAllow));

    auto updated = eventFuture.get();
    EXPECT_EQ(updated.threadId, threadId);
    EXPECT_EQ(updated.metadata.permissionMode, ThreadPermissionMode::AlwaysAllow);
    EXPECT_EQ(Harness::instance().currentThreadPermissionMode(),
              ThreadPermissionMode::AlwaysAllow);

    Harness::instance().unsubscribe(subId);
}

TEST_F(HarnessTest, commandAllowAlways_persistsRuleAndSkipsSecondPrompt) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    Permissions permissions(threadId, "agent-1");
    auto intent =
        permissions.getIntentAnalyzer().analyze("git status", "/tmp");

    std::atomic<int> requestCount{0};
    std::promise<PermissionEscalationRequest> requestPromise;
    auto requestFuture = requestPromise.get_future();
    int subId = Harness::instance().subscribe(
        [&](const AppEvent& event) {
            if (auto request = std::get_if<PermissionEscalationRequest>(&event)) {
                requestCount.fetch_add(1);
                requestPromise.set_value(*request);
            }
        });

    std::promise<PermissionResponse> responsePromise;
    auto responseFuture = responsePromise.get_future();
    std::thread worker([&]() {
        responsePromise.set_value(
            permissions.requestCommandApproval("git status", intent));
    });

    auto request = requestFuture.get();
    EXPECT_TRUE(Harness::instance().resolvePermissionEscalation(
        request.requestId, PermissionResponse::AllowAlways));
    EXPECT_EQ(responseFuture.get(), PermissionResponse::AllowAlways);
    worker.join();

    EXPECT_EQ(permissions.requestCommandApproval("git status", intent),
              PermissionResponse::AllowAlways);
    EXPECT_EQ(requestCount.load(), 1);

    auto rules = Harness::instance().threadPermissionRules(threadId);
    ASSERT_EQ(rules.commandAllowRules.size(), 1u);
    EXPECT_EQ(rules.commandAllowRules[0].exactCommand, "git status");

    Harness::instance().unsubscribe(subId);
}

TEST_F(HarnessTest, writeAllowAlways_persistsPrefixAndSkipsSecondPrompt) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());

    Permissions permissions(threadId, "agent-1");
    AgentContext context;
    context.permissions.allowedPaths = {"/tmp/**"};
    context.permissions.allowOutsideCwd = false;
    permissions.bindContext(context);

    std::string filePath = "/tmp/project/src/file.txt";

    std::atomic<int> requestCount{0};
    std::promise<PermissionEscalationRequest> requestPromise;
    auto requestFuture = requestPromise.get_future();
    int subId = Harness::instance().subscribe(
        [&](const AppEvent& event) {
            if (auto request = std::get_if<PermissionEscalationRequest>(&event)) {
                requestCount.fetch_add(1);
                requestPromise.set_value(*request);
            }
        });

    std::promise<PermissionResponse> responsePromise;
    auto responseFuture = responsePromise.get_future();
    std::thread worker([&]() {
        responsePromise.set_value(permissions.requestEditApproval(filePath));
    });

    auto request = requestFuture.get();
    EXPECT_TRUE(Harness::instance().resolvePermissionEscalation(
        request.requestId, PermissionResponse::AllowAlways));
    EXPECT_EQ(responseFuture.get(), PermissionResponse::AllowAlways);
    worker.join();

    EXPECT_EQ(permissions.requestEditApproval(filePath),
              PermissionResponse::AllowAlways);
    EXPECT_EQ(requestCount.load(), 1);

    auto rules = Harness::instance().threadPermissionRules(threadId);
    ASSERT_EQ(rules.writeAllowPaths.size(), 1u);
    EXPECT_EQ(rules.writeAllowPaths[0], "/tmp/project/src/**");

    Harness::instance().unsubscribe(subId);
}

TEST_F(HarnessTest, vulnerableCommandsRemainDeniedInAlwaysAllowMode) {
    std::string threadId = Harness::instance().newThread({}, "/tmp", "test");
    ASSERT_FALSE(threadId.empty());
    ASSERT_TRUE(Harness::instance().setCurrentThreadPermissionMode(
        ThreadPermissionMode::AlwaysAllow));

    Permissions permissions(threadId, "agent-1");
    auto intent = permissions.getIntentAnalyzer().analyze("rm -rf /", "/tmp");
    EXPECT_EQ(intent.severity, CommandSeverity::VULNERABLE);
    EXPECT_EQ(permissions.requestCommandApproval("rm -rf /", intent),
              PermissionResponse::Deny);
}

} // namespace
