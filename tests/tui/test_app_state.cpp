#include <gtest/gtest.h>

#include "AppState.hpp"
#include "HarnessEvents.hpp"
#include "Message.hpp"
#include "Metrics.hpp"

using namespace firmius::tui;
using namespace firmius::harness;
using namespace firmius::shared;

class AppStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        state_ = std::make_shared<AppState>();
    }

    std::shared_ptr<AppState> state_;
};

TEST_F(AppStateTest, InitialStateHasEmptyMessages) {
    auto messages = state_->getMessages();
    EXPECT_TRUE(messages.empty());
}

TEST_F(AppStateTest, InitialStateHasNoStreamingMessage) {
    auto streaming = state_->getStreamingMessage();
    EXPECT_FALSE(streaming.has_value());
}

TEST_F(AppStateTest, InitialStateHasIdleStatus) {
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.status, AgentStatus::Idle);
}

TEST_F(AppStateTest, ThreadChangedClearsMessages) {
    ThreadChanged event;
    event.threadId = "thread-123";
    event.metadata.title = "Test Thread";
    
    state_->applyEvent(event);
    
    auto messages = state_->getMessages();
    EXPECT_TRUE(messages.empty());
}

TEST_F(AppStateTest, ThreadChangedUpdatesFooter) {
    ThreadChanged event;
    event.threadId = "thread-456";
    event.metadata.title = "My Thread";
    
    state_->applyEvent(event);
    
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.threadId, "thread-456");
    EXPECT_EQ(footer.threadTitle, "My Thread");
}

TEST_F(AppStateTest, MessageChunkCreatesStreamingMessage) {
    MessageChunk chunk;
    chunk.agentId = "agent-1";
    chunk.delta = "Hello";
    chunk.isThinking = false;
    
    state_->applyEvent(chunk);
    
    auto streaming = state_->getStreamingMessage();
    ASSERT_TRUE(streaming.has_value());
    EXPECT_EQ(streaming->agentId, "agent-1");
}

TEST_F(AppStateTest, MessageChunkUpdatesStatusToStreaming) {
    MessageChunk chunk;
    chunk.agentId = "agent-1";
    chunk.delta = "Hello";
    chunk.isThinking = false;
    
    state_->applyEvent(chunk);
    
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.status, AgentStatus::Streaming);
}

TEST_F(AppStateTest, MultipleMessageChunksAccumulate) {
    MessageChunk chunk1;
    chunk1.agentId = "agent-1";
    chunk1.delta = "Hello ";
    chunk1.isThinking = false;
    
    MessageChunk chunk2;
    chunk2.agentId = "agent-1";
    chunk2.delta = "world";
    chunk2.isThinking = false;
    
    state_->applyEvent(chunk1);
    state_->applyEvent(chunk2);
    
    auto streaming = state_->getStreamingMessage();
    ASSERT_TRUE(streaming.has_value());
    EXPECT_EQ(streaming->message.content.size(), 2u);
}

TEST_F(AppStateTest, MessageChunkWithThinking) {
    MessageChunk chunk;
    chunk.agentId = "agent-1";
    chunk.delta = "Thinking...";
    chunk.isThinking = true;
    
    state_->applyEvent(chunk);
    
    auto streaming = state_->getStreamingMessage();
    ASSERT_TRUE(streaming.has_value());
    EXPECT_FALSE(streaming->message.content.empty());
}

TEST_F(AppStateTest, MessageCompletedAddsToMessages) {
    MessageCompleted completed;
    completed.agentId = "agent-1";
    completed.message.id = "msg-1";
    completed.message.role = Role::Assistant;
    completed.message.timestamp = 12345;
    
    state_->applyEvent(completed);
    
    auto messages = state_->getMessages();
    EXPECT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].message.id, "msg-1");
}

TEST_F(AppStateTest, MessageCompletedClearsStreaming) {
    MessageChunk chunk;
    chunk.agentId = "agent-1";
    chunk.delta = "Hello";
    chunk.isThinking = false;
    state_->applyEvent(chunk);
    
    MessageCompleted completed;
    completed.agentId = "agent-1";
    completed.message.id = "msg-1";
    completed.message.role = Role::Assistant;
    state_->applyEvent(completed);
    
    auto streaming = state_->getStreamingMessage();
    EXPECT_FALSE(streaming.has_value());
}

TEST_F(AppStateTest, MessageCompletedUpdatesStatusToIdle) {
    MessageChunk chunk;
    chunk.agentId = "agent-1";
    chunk.delta = "Hello";
    state_->applyEvent(chunk);
    
    MessageCompleted completed;
    completed.agentId = "agent-1";
    completed.message.id = "msg-1";
    completed.message.role = Role::Assistant;
    state_->applyEvent(completed);
    
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.status, AgentStatus::Idle);
}

TEST_F(AppStateTest, ToolCallStartedAddsToActive) {
    ToolCallStarted started;
    started.agentId = "agent-1";
    started.toolCallId = "call-1";
    started.name = "bash";
    started.args = R"({"command":"ls"})";
    
    state_->applyEvent(started);
    
    auto toolCalls = state_->getActiveToolCalls();
    EXPECT_EQ(toolCalls.size(), 1u);
    EXPECT_EQ(toolCalls[0].name, "bash");
}

TEST_F(AppStateTest, ToolCallStartedUpdatesStatusToExecutingTool) {
    ToolCallStarted started;
    started.agentId = "agent-1";
    started.toolCallId = "call-1";
    started.name = "bash";
    started.args = "{}";
    
    state_->applyEvent(started);
    
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.status, AgentStatus::ExecutingTool);
}

TEST_F(AppStateTest, ToolCallResultRemovesFromActive) {
    ToolCallStarted started;
    started.agentId = "agent-1";
    started.toolCallId = "call-1";
    started.name = "bash";
    started.args = "{}";
    state_->applyEvent(started);
    
    ToolCallResult result;
    result.agentId = "agent-1";
    result.toolCallId = "call-1";
    result.result = "output";
    result.success = true;
    state_->applyEvent(result);
    
    auto toolCalls = state_->getActiveToolCalls();
    EXPECT_TRUE(toolCalls.empty());
}

TEST_F(AppStateTest, ToolCallResultLastToolUpdatesStatusToIdle) {
    ToolCallStarted started;
    started.agentId = "agent-1";
    started.toolCallId = "call-1";
    started.name = "bash";
    state_->applyEvent(started);
    
    ToolCallResult result;
    result.agentId = "agent-1";
    result.toolCallId = "call-1";
    result.result = "output";
    result.success = true;
    state_->applyEvent(result);
    
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.status, AgentStatus::Idle);
}

TEST_F(AppStateTest, SubagentSpawnedAddsSubagent) {
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.persona = "coder";
    spawned.friendlyName = "Coder Agent";
    spawned.title = "Fixing bug";
    
    state_->applyEvent(spawned);
    
    auto subagents = state_->getSubagents();
    EXPECT_EQ(subagents.size(), 1u);
    EXPECT_EQ(subagents[0].agentId, "sub-1");
    EXPECT_EQ(subagents[0].persona, "coder");
}

TEST_F(AppStateTest, ThreadLockedCreatesNotification) {
    ThreadLocked locked;
    locked.threadId = "thread-1";
    locked.ownerPid = 12345;
    
    state_->applyEvent(locked);
    
    auto notifications = state_->getNotifications();
    EXPECT_EQ(notifications.size(), 1u);
    EXPECT_NE(notifications[0].find("locked"), std::string::npos);
}

TEST_F(AppStateTest, ThreadLockedUpdatesStatusToError) {
    ThreadLocked locked;
    locked.threadId = "thread-1";
    locked.ownerPid = 12345;
    
    state_->applyEvent(locked);
    
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.status, AgentStatus::Error);
}

TEST_F(AppStateTest, HarnessErrorCreatesNotification) {
    HarnessError error;
    error.message = "Something went wrong";
    
    state_->applyEvent(error);
    
    auto notifications = state_->getNotifications();
    EXPECT_EQ(notifications.size(), 1u);
    EXPECT_EQ(notifications[0], "Something went wrong");
}

TEST_F(AppStateTest, HarnessErrorAddsErrorMessage) {
    HarnessError error;
    error.message = "Something went wrong";
    
    state_->applyEvent(error);
    
    auto messages = state_->getMessages();
    EXPECT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].type, ChatMessageType::Error);
}

TEST_F(AppStateTest, AgentCompactingUpdatesStatus) {
    AgentCompactingEvent compacting;
    compacting.agentId = "agent-1";
    
    state_->applyEvent(compacting);
    
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.status, AgentStatus::Compacting);
    EXPECT_TRUE(footer.isCompacting);
}

TEST_F(AppStateTest, ContextCompactedUpdatesTokensSaved) {
    ContextCompactedEvent compacted;
    compacted.agentId = "agent-1";
    compacted.tokensSaved = 500;
    
    state_->applyEvent(compacted);
    
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.tokensSaved, 500u);
    EXPECT_FALSE(footer.isCompacting);
}

TEST_F(AppStateTest, ContextCompactedAddsCompactionMessage) {
    ContextCompactedEvent compacted;
    compacted.agentId = "agent-1";
    compacted.tokensSaved = 500;
    
    state_->applyEvent(compacted);
    
    auto messages = state_->getMessages();
    EXPECT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].type, ChatMessageType::Compaction);
    EXPECT_EQ(messages[0].tokensSaved, 500u);
}

TEST_F(AppStateTest, ThreadDeletedClearsCurrentThread) {
    ThreadChanged changed;
    changed.threadId = "thread-1";
    state_->applyEvent(changed);
    
    ThreadDeleted deleted;
    deleted.threadId = "thread-1";
    state_->applyEvent(deleted);
    
    auto footer = state_->getFooterInfo();
    EXPECT_TRUE(footer.threadId.empty());
}

TEST_F(AppStateTest, ModelSwitchedUpdatesSubagent) {
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.persona = "coder";
    spawned.friendlyName = "Coder";
    state_->applyEvent(spawned);
    
    ModelSwitchedEvent switched;
    switched.agentId = "sub-1";
    switched.newProviderId = "openai";
    switched.newModelId = "gpt-4";
    state_->applyEvent(switched);
    
    auto subagents = state_->getSubagents();
    EXPECT_EQ(subagents[0].providerId, "openai");
    EXPECT_EQ(subagents[0].modelId, "gpt-4");
}

TEST_F(AppStateTest, HistoryUndoneCreatesNotification) {
    HistoryUndoneEvent undone;
    undone.threadId = "thread-1";
    undone.agentId = "agent-1";
    undone.turnsRemoved = 3;
    undone.compactionReversed = false;
    
    state_->applyEvent(undone);
    
    auto notifications = state_->getNotifications();
    EXPECT_EQ(notifications.size(), 1u);
    EXPECT_NE(notifications[0].find("3"), std::string::npos);
}

TEST_F(AppStateTest, ThreadTitleUpdatedUpdatesFooter) {
    ThreadChanged changed;
    changed.threadId = "thread-1";
    state_->applyEvent(changed);
    
    ThreadTitleUpdated updated;
    updated.threadId = "thread-1";
    updated.title = "New Title";
    state_->applyEvent(updated);
    
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.threadTitle, "New Title");
}

TEST_F(AppStateTest, MessageQueuedAddsTag) {
    MessageQueued queued;
    queued.messageId = "msg-1";
    queued.text = "Hello";
    
    state_->applyEvent(queued);
    
    auto tags = state_->getQueuedMessageTags();
    EXPECT_EQ(tags.count("msg-1"), 1u);
}

TEST_F(AppStateTest, MessageDequeuedRemovesTag) {
    MessageQueued queued;
    queued.messageId = "msg-1";
    state_->applyEvent(queued);
    
    MessageDequeued dequeued;
    dequeued.messageId = "msg-1";
    state_->applyEvent(dequeued);
    
    auto tags = state_->getQueuedMessageTags();
    EXPECT_EQ(tags.count("msg-1"), 0u);
}

TEST_F(AppStateTest, AgentRetryingCreatesNotification) {
    AgentRetrying retrying;
    retrying.agentId = "agent-1";
    retrying.attempt = 2;
    retrying.maxAttempts = 3;
    retrying.delayMs = 1000;
    
    state_->applyEvent(retrying);
    
    auto notifications = state_->getNotifications();
    EXPECT_EQ(notifications.size(), 1u);
    EXPECT_NE(notifications[0].find("retrying"), std::string::npos);
}

TEST_F(AppStateTest, AgentRetryingAddsRetryMessage) {
    AgentRetrying retrying;
    retrying.agentId = "agent-1";
    retrying.attempt = 2;
    retrying.maxAttempts = 3;
    retrying.delayMs = 1000;
    
    state_->applyEvent(retrying);
    
    auto messages = state_->getMessages();
    EXPECT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].type, ChatMessageType::Retry);
    EXPECT_EQ(messages[0].attempt, 2);
    EXPECT_EQ(messages[0].maxAttempts, 3);
}

TEST_F(AppStateTest, AgentRetryFailedUpdatesStatus) {
    AgentRetryFailed failed;
    failed.agentId = "agent-1";
    failed.reason = "Max retries exceeded";
    
    state_->applyEvent(failed);
    
    auto footer = state_->getFooterInfo();
    EXPECT_EQ(footer.status, AgentStatus::Error);
}

TEST_F(AppStateTest, FocusedAgentIdDefaultsToEmpty) {
    EXPECT_TRUE(state_->getFocusedAgentId().empty());
}

TEST_F(AppStateTest, SetFocusedAgentIdWorks) {
    state_->setFocusedAgentId("agent-1");
    EXPECT_EQ(state_->getFocusedAgentId(), "agent-1");
}

TEST_F(AppStateTest, SubagentSpawnedSetsFriendlyNameOnCompletion) {
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.friendlyName = "My Agent";
    state_->applyEvent(spawned);
    
    MessageCompleted completed;
    completed.agentId = "sub-1";
    completed.message.id = "msg-1";
    completed.message.role = Role::Assistant;
    state_->applyEvent(completed);
    
    auto messages = state_->getMessages();
    EXPECT_EQ(messages[0].friendlyName, "My Agent");
}
