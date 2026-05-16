#include "EventRouter.hpp"
#include "AppState.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

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

  router.routeRuntimeEvent("user_message_sent", R"({"text":"Hello"})", "t1", "a1");
  EXPECT_EQ(state.itemCount(), 1u);

  // Deduplication
  router.routeRuntimeEvent("user_message_sent", R"({"text":"Hello"})", "t1", "a1");
  EXPECT_EQ(state.itemCount(), 1u);
}

TEST(EventRouterTest, MessageQueueCount) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent("message_queued", "{}", "t1", "");
  EXPECT_EQ(state.queuedMessageCount(), 1);

  router.routeRuntimeEvent("message_dequeued", "{}", "t1", "");
  EXPECT_EQ(state.queuedMessageCount(), 0);

  // Shouldn't go below zero
  router.routeRuntimeEvent("message_dequeued", "{}", "t1", "");
  EXPECT_EQ(state.queuedMessageCount(), 0);
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
