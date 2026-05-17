#include "AppState.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

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
