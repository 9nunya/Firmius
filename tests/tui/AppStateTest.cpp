#include "AppState.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(AppStateTest, SetThreadAndDirtyFlag) {
  AppState state;
  EXPECT_TRUE(state.isDirty());
  state.clearDirty();
  EXPECT_FALSE(state.isDirty());

  state.setThreadId("thread-123");
  EXPECT_EQ(state.threadId(), "thread-123");
  EXPECT_TRUE(state.isDirty());
}

TEST(AppStateTest, ConnectionStatus) {
  AppState state;
  EXPECT_EQ(state.connectionStatus(), ConnectionStatus::Disconnected);
  state.setConnectionStatus(ConnectionStatus::Connected);
  EXPECT_EQ(state.connectionStatus(), ConnectionStatus::Connected);
}

TEST(AppStateTest, ItemLifecycle) {
  AppState state;
  state.clearDirty();

  state.addItem(std::make_unique<UserMessageItem>("Hello"));
  EXPECT_EQ(state.itemCount(), 1u);
  EXPECT_TRUE(state.isDirty());
}

TEST(AppStateTest, StreamingItems) {
  AppState state;
  auto item = std::make_unique<AgentTextItem>();
  auto* ptr = item.get();
  state.addItem(std::move(item));
  state.setActiveTextItem(ptr);

  EXPECT_EQ(state.activeTextItem(), ptr);
  EXPECT_FALSE(ptr->isFinalized());

  ptr->appendDelta("Hello ");
  ptr->appendDelta("world!");
  EXPECT_EQ(ptr->accumulated(), "Hello world!");

  ptr->finalize();
  EXPECT_TRUE(ptr->isFinalized());
  state.setActiveTextItem(nullptr);
  EXPECT_EQ(state.activeTextItem(), nullptr);
}

TEST(AppStateTest, ToolCallLifecycle) {
  AppState state;
  auto item = std::make_unique<ToolCallItem>("call-1", "Process", "agent-1");
  state.addItem(std::move(item));

  auto* tc = state.findToolCallById("call-1");
  ASSERT_NE(tc, nullptr);
  EXPECT_EQ(tc->phase(), ToolPhase::Preparing);

  tc->setArgs(R"({"action":"Execute","command":"ls"})");
  tc->setPhase(ToolPhase::Called);
  EXPECT_EQ(tc->phase(), ToolPhase::Called);

  tc->setResult(true, R"({"exit_code":0})");
  EXPECT_EQ(tc->phase(), ToolPhase::FinishedSuccess);
  EXPECT_TRUE(tc->success());

  // Not found
  EXPECT_EQ(state.findToolCallById("nonexistent"), nullptr);
}

TEST(AppStateTest, LiveItemTracking) {
  AppState state;
  auto item = std::make_unique<ToolCallItem>("call-1", "Process", "agent-1");
  item->setArgs(R"({"action":"Execute","command":"sleep 10"})");
  item->setLive(true);
  state.addItem(std::move(item));

  EXPECT_TRUE(state.hasLiveItems());

  state.markLiveItemsDirty();
  auto* tc = state.findToolCallById("call-1");
  ASSERT_NE(tc, nullptr);
  EXPECT_TRUE(tc->needsRender());
}

TEST(AppStateTest, InputBuffer) {
  AppState state;
  state.setInputBuffer("test");
  EXPECT_EQ(state.inputBuffer(), "test");

  state.appendToInput('!');
  EXPECT_EQ(state.inputBuffer(), "test!");

  state.backspaceInput();
  EXPECT_EQ(state.inputBuffer(), "test");

  state.clearInput();
  EXPECT_EQ(state.inputBuffer(), "");
}

TEST(AppStateTest, ActivityContext) {
  AppState state;
  EXPECT_EQ(state.activityContext(), ActivityContext::Idle);

  // Any non-inert agent status makes the context Active.
  state.setAgentStatus(firmius::shared::AgentStatus::Streaming);
  EXPECT_EQ(state.activityContext(), ActivityContext::Active);

  state.setAgentStatus(firmius::shared::AgentStatus::ExecutingTool);
  EXPECT_EQ(state.activityContext(), ActivityContext::Active);

  state.setAgentStatus(firmius::shared::AgentStatus::ProviderWaiting);
  EXPECT_EQ(state.activityContext(), ActivityContext::Active);

  // Idle/Cancelled/Error are inert.
  state.setAgentStatus(firmius::shared::AgentStatus::Idle);
  EXPECT_EQ(state.activityContext(), ActivityContext::Idle);

  state.setAgentStatus(firmius::shared::AgentStatus::Cancelled);
  EXPECT_EQ(state.activityContext(), ActivityContext::Idle);

  state.setAgentStatus(firmius::shared::AgentStatus::Error);
  EXPECT_EQ(state.activityContext(), ActivityContext::Idle);

  PendingPermission perm;
  perm.title = "test";
  state.pushPendingPermission(perm);
  EXPECT_EQ(state.activityContext(), ActivityContext::PermissionPending);
}

TEST(AppStateTest, QueueMessageIdsTrackCounts) {
  AppState state;
  state.setQueuedMessageCount(0);

  state.queueMessageId("m1");
  state.queueMessageId("m2");
  EXPECT_EQ(state.queuedMessageCount(), 2);
  EXPECT_TRUE(state.isMessageQueued("m1"));
  EXPECT_TRUE(state.isMessageQueued("m2"));

  state.dequeueMessageId("m1");
  EXPECT_EQ(state.queuedMessageCount(), 1);
  EXPECT_FALSE(state.isMessageQueued("m1"));
  EXPECT_TRUE(state.isMessageQueued("m2"));
}

TEST(AppStateTest, QueuedUserMessagesTrackByAgent) {
  AppState state;

  state.upsertQueuedUserMessage({"m1", "first", "lead"});
  state.upsertQueuedUserMessage({"m2", "second", "lead"});
  state.upsertQueuedUserMessage({"m3", "other", "sub"});

  auto lead = state.queuedUserMessagesForAgent("lead");
  ASSERT_EQ(lead.size(), 2u);
  EXPECT_EQ(lead[0].messageId, "m1");
  EXPECT_EQ(lead[1].messageId, "m2");

  state.removeQueuedUserMessage("m1");
  lead = state.queuedUserMessagesForAgent("lead");
  ASSERT_EQ(lead.size(), 1u);
  EXPECT_EQ(lead[0].messageId, "m2");
}

TEST(AppStateTest, ItemSpans) {
  AppState state;
  EXPECT_TRUE(state.itemSpans().empty());

  std::vector<ItemSpan> spans = {{0, 1, 3}, {1, 4, 2}};
  state.setItemSpans(spans);
  EXPECT_EQ(state.itemSpans().size(), 2u);
  EXPECT_EQ(state.itemSpans()[0].itemIndex, 0u);
  EXPECT_EQ(state.itemSpans()[0].terminalRow, 1);
  EXPECT_EQ(state.itemSpans()[0].rowCount, 3);

  state.clearItemSpans();
  EXPECT_TRUE(state.itemSpans().empty());
}

TEST(AppStateTest, TodoVisibilityAndStorage) {
  AppState state;
  state.setAgentId("agent-1");
  state.focusAgent("agent-1");

  firmius::shared::TodoItem item;
  item.id = 1;
  item.text = "Ship the bottom bar";
  item.status = firmius::shared::TodoStatus::InProgress;

  state.setAgentTodos("agent-1", {item});
  ASSERT_EQ(state.focusedAgentTodos().size(), 1u);
  EXPECT_EQ(state.focusedAgentTodos()[0].text, "Ship the bottom bar");

  EXPECT_TRUE(state.todoVisible());
  state.toggleTodoVisibility();
  EXPECT_FALSE(state.todoVisible());
}

TEST(AppStateTest, WelcomeStateClearsAgentTabsWithoutDroppingDefaults) {
  AppState state;
  state.setModelLabel("gitlawb/mimo-v2.5-pro");
  state.setAgentPurpose("lead");
  state.setThreadId("thread-1");
  auto& agent = state.getOrCreateAgent("agent-1");
  agent.agentId = "agent-1";
  auto& agent2 = state.getOrCreateAgent("agent-2");
  agent2.agentId = "agent-2";

  EXPECT_TRUE(state.hasMultipleAgents());

  state.resetWelcomeState();

  EXPECT_FALSE(state.hasMultipleAgents());
  EXPECT_TRUE(state.threadId().empty());
  EXPECT_TRUE(state.agentId().empty());
  EXPECT_EQ(state.modelLabel(), "gitlawb/mimo-v2.5-pro");
  EXPECT_EQ(state.agentPurpose(), "lead");
}

TEST(AppStateTest, DaemonReadyFlag) {
  AppState state;
  EXPECT_FALSE(state.daemonReady());
  state.setDaemonReady(true);
  EXPECT_TRUE(state.daemonReady());
}
