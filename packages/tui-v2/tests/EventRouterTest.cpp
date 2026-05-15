#include "EventRouter.hpp"
#include "AppState.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(EventRouterTest, AgentTextAppendsToStreaming) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent("agent_text", R"({"delta":"Hi"})", "thread1", "agent1");
  EXPECT_EQ(state.currentStreamingText(), "Hi");

  router.routeRuntimeEvent("agent_text", R"({"delta":" there"})", "thread1", "agent1");
  EXPECT_EQ(state.currentStreamingText(), "Hi there");
}

TEST(EventRouterTest, AgentToolCallAddsActiveCall) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent("agent_tool_call",
                           R"({"toolCallId":"call_123","toolName":"read_file"})",
                           "thread1", "agent1");

  auto calls = state.activeToolCalls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].toolCallId, "call_123");
  EXPECT_EQ(calls[0].toolName, "read_file");
  EXPECT_EQ(calls[0].agentId, "agent1");

  EXPECT_EQ(state.transcriptLineCount(), 1u);
  EXPECT_EQ(state.transcriptLines()[0].kind, TranscriptLine::Kind::ToolCall);
}

TEST(EventRouterTest, AgentFinishedFinalizesStreamingAndSetsIdle) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent("agent_text", R"({"delta":"Hi"})", "t1", "a1");
  EXPECT_EQ(state.currentStreamingText(), "Hi");
  EXPECT_TRUE(state.isStreaming());

  router.routeRuntimeEvent("agent_finished", "{}", "t1", "a1");
  EXPECT_EQ(state.currentStreamingText(), "");
  EXPECT_FALSE(state.isStreaming());
  EXPECT_EQ(state.agentStatus(), firmius::shared::AgentStatus::Idle);
  EXPECT_EQ(state.transcriptLineCount(), 1u);
}

TEST(EventRouterTest, UserMessageSentAppendsToTranscript) {
  AppState state;
  EventRouter router(state);

  router.routeRuntimeEvent("user_message_sent", R"({"text":"Hello agent"})", "t1", "a1");

  EXPECT_EQ(state.transcriptLineCount(), 1u);
  EXPECT_EQ(state.transcriptLines()[0].kind, TranscriptLine::Kind::UserMessage);
  EXPECT_EQ(state.transcriptLines()[0].text, "Hello agent");
}

TEST(EventRouterTest, MessageQueueCount) {
  AppState state;
  EventRouter router(state);

  EXPECT_EQ(state.queuedMessageCount(), 0);

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
                           R"({"requestId":"req_1","title":"Approve Action","message":"...","toolName":"del","allowAlways":true})",
                           "t1", "a1");

  auto perm = state.pendingPermission();
  ASSERT_TRUE(perm.has_value());
  EXPECT_EQ(perm->requestId, "req_1");
  EXPECT_EQ(perm->title, "Approve Action");
  EXPECT_EQ(perm->toolName, "del");
  EXPECT_TRUE(perm->allowAlways);

  router.routeRuntimeEvent("permission_escalation_resolved", "{}", "t1", "a1");
  EXPECT_FALSE(state.pendingPermission().has_value());
}
