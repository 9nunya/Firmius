#include "EventRouter.hpp"
#include "AppState.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(EventRouterTest, AgentTextCreatesTextItem) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent("agent_text", R"({"delta":"Hi"})", "t1", "a1");
  EXPECT_EQ(state.itemCount(), 1u);
  EXPECT_NE(state.activeTextItem(), nullptr);
  EXPECT_EQ(state.activeTextItem()->accumulated(), "Hi");

  router.routeRuntimeEvent("agent_text", R"({"delta":" there"})", "t1", "a1");
  EXPECT_EQ(state.itemCount(), 1u);
  EXPECT_EQ(state.activeTextItem()->accumulated(), "Hi there");
}

TEST(EventRouterTest, AgentToolCallCreatesToolCallItem) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent("agent_tool_call",
      R"({"toolCallId":"call_123","toolName":"read_file","toolArgs":"{\"path\":\"foo.cpp\"}"})",
      "t1", "a1");

  EXPECT_EQ(state.itemCount(), 1u);
  auto* tc = state.findToolCallById("call_123");
  ASSERT_NE(tc, nullptr);
  EXPECT_EQ(tc->toolName(), "read_file");
  EXPECT_EQ(tc->phase(), ToolPhase::Called);
}

TEST(EventRouterTest, AgentTextAfterToolCallStaysAboveCurrentTurnTools) {
  AppState state;
  EventRouter router(state);
  auto& agent = state.getOrCreateAgent("a1");
  agent.currentTurn = AgentTurnState{};
  agent.currentTurn->agentId = "a1";

  router.routeRuntimeEvent(
      "agent_tool_call",
      R"({"toolCallId":"call_123","toolName":"Read","toolArgs":"{\"path\":\"foo.cpp\"}"})",
      "t1", "a1");
  router.routeRuntimeEvent("agent_text", R"({"delta":"I should explain this first."})",
                           "t1", "a1");

  ASSERT_EQ(state.itemCount(), 2u);
  EXPECT_EQ(state.items()[0]->type(), "AgentText");
  EXPECT_EQ(state.items()[1]->type(), "ToolCall");
}

TEST(EventRouterTest, AgentFileEditedAttachesDiff) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent("agent_tool_call",
      R"({"toolCallId":"call-1","toolName":"Edit","toolArgs":"{\"patch\":\"...\"}"})",
      "t1", "a1");

  router.routeRuntimeEvent("agent_file_edited",
      R"({"toolCallId":"call-1","path":"foo.cpp","diffPreview":"--- a/foo.cpp\n+++ b/foo.cpp\n","addedLines":1,"removedLines":0})",
      "", "");

  auto* tc = state.findToolCallById("call-1");
  ASSERT_NE(tc, nullptr);
  EXPECT_EQ(tc->diffEdits().size(), 1u);
  EXPECT_EQ(tc->diffEdits()[0].path, "foo.cpp");
}

TEST(EventRouterTest, AgentTurnCompletedFinalizesItems) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent("agent_text", R"({"delta":"Hi"})", "t1", "a1");
  EXPECT_NE(state.activeTextItem(), nullptr);

  router.routeRuntimeEvent("agent_turn_completed", "{}", "t1", "a1");
  EXPECT_EQ(state.activeTextItem(), nullptr);
}

TEST(EventRouterTest, UserMessageSentAppendsItem) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent(
      "user_message_sent",
      R"({"messageId":"m1","text":"Hello","agentId":"a1"})", "t1", "a1");
  EXPECT_EQ(state.itemCount(), 1u);

  // Deduplication
  router.routeRuntimeEvent(
      "user_message_sent",
      R"({"messageId":"m1","text":"Hello","agentId":"a1"})", "t1", "a1");
  EXPECT_EQ(state.itemCount(), 1u);
}

TEST(EventRouterTest, MessageQueueCount) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent(
      "message_queued",
      R"({"messageId":"m1","text":"queued","agentId":"lead"})", "t1", "");
  EXPECT_EQ(state.queuedMessageCount(), 1);
  EXPECT_TRUE(state.isMessageQueued("m1"));
  ASSERT_EQ(state.itemCount(), 1u);
  auto* queued = static_cast<UserMessageItem*>(state.items().front().get());
  EXPECT_TRUE(queued->queued());
  EXPECT_EQ(queued->agentId(), "lead");

  router.routeRuntimeEvent("message_dequeued", R"({"messageId":"m1"})", "t1", "");
  EXPECT_EQ(state.queuedMessageCount(), 0);
  EXPECT_FALSE(state.isMessageQueued("m1"));
  EXPECT_FALSE(queued->queued());

  // Shouldn't go below zero
  router.routeRuntimeEvent("message_dequeued", R"({"messageId":"m1"})", "t1", "");
  EXPECT_EQ(state.queuedMessageCount(), 0);
}

TEST(EventRouterTest, DequeuedMessageBecomesSentWithoutDuplication) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent(
      "message_queued",
      R"({"messageId":"m1","text":"queued","agentId":"lead"})", "t1", "");
  router.routeRuntimeEvent("message_dequeued", R"({"messageId":"m1"})", "t1",
                           "");
  router.routeRuntimeEvent(
      "user_message_sent",
      R"({"messageId":"m1","text":"queued","agentId":"lead"})", "t1",
      "lead");

  ASSERT_EQ(state.itemCount(), 1u);
  auto* queued = static_cast<UserMessageItem*>(state.items().front().get());
  EXPECT_EQ(queued->messageId(), "m1");
  EXPECT_FALSE(queued->queued());
}

TEST(EventRouterTest, ModelSwitchedUpdatesAgentState) {
  AppState state;
  EventRouter router(state);
  auto& agent = state.getOrCreateAgent("a1");
  agent.agentId = "a1";
  agent.providerId = "old";
  agent.modelId = "old-model";
  state.setAgentId("a1");
  state.focusAgent("a1");

  router.routeRuntimeEvent(
      "model_switched",
      R"({"agentId":"a1","newProviderId":"openai","newModelId":"gpt-5.4"})",
      "t1", "a1");

  EXPECT_EQ(state.modelLabel(), "openai/gpt-5.4");
  const auto* updated = state.findAgentState("a1");
  ASSERT_NE(updated, nullptr);
  EXPECT_EQ(updated->providerId, "openai");
  EXPECT_EQ(updated->modelId, "gpt-5.4");
  EXPECT_EQ(updated->contextUsedTokens, 0u);
}

TEST(EventRouterTest, PermissionEscalation) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent("permission_escalation_request",
      R"({"requestId":"req_1","title":"Approve","message":"...","toolName":"del","allowAlways":true})",
      "t1", "a1");

  auto perm = state.pendingPermission();
  ASSERT_TRUE(perm.has_value());
  EXPECT_EQ(perm->requestId, "req_1");

  router.routeRuntimeEvent("permission_escalation_resolved",
      R"({"requestId":"req_1"})", "t1", "a1");
  EXPECT_FALSE(state.pendingPermission().has_value());
}

TEST(EventRouterTest, AgentTodoUpdatedHydratesState) {
  AppState state;
  EventRouter router(state);
  state.setAgentId("a1");
  state.focusAgent("a1");

  router.routeRuntimeEvent(
      "agent_todo_updated",
      R"({"agentId":"a1","todo":{"thread_id":"t1","agent_id":"a1","next_id":2,"items":[{"id":1,"text":"Polish status bar","status":"InProgress","chunk_id":"","plan_id":"","created_at":0,"updated_at":0}]}})",
      "t1", "a1");

  ASSERT_EQ(state.focusedAgentTodos().size(), 1u);
  EXPECT_EQ(state.focusedAgentTodos()[0].text, "Polish status bar");
}
