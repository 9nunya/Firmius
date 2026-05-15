#include "AppState.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(AppStateTest, SetThreadAndDirtyFlag) {
  AppState state;
  EXPECT_TRUE(state.isDirty()); // Initially dirty
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

TEST(AppStateTest, TranscriptLines) {
  AppState state;
  TranscriptLine line;
  line.text = "Hello";
  state.appendTranscriptLine(std::move(line));

  EXPECT_EQ(state.transcriptLineCount(), 1u);
  EXPECT_EQ(state.transcriptLines()[0].text, "Hello");

  std::vector<TranscriptLine> lines;
  lines.push_back({TranscriptLine::Kind::System, "Reset", "", "", "", true});
  state.setTranscriptLines(std::move(lines));
  EXPECT_EQ(state.transcriptLineCount(), 1u);
  EXPECT_EQ(state.transcriptLines()[0].text, "Reset");
  EXPECT_EQ(state.lastRenderedLineIndex(), 0u);
}

TEST(AppStateTest, StreamingAccumulation) {
  AppState state;
  state.appendStreamingDelta("Hello ");
  state.appendStreamingDelta("world!");

  EXPECT_EQ(state.currentStreamingText(), "Hello world!");
  EXPECT_TRUE(state.isStreaming());
  EXPECT_EQ(state.agentStatus(), firmius::shared::AgentStatus::Streaming);

  state.finalizeStreamingLine();
  EXPECT_EQ(state.currentStreamingText(), "");
  EXPECT_EQ(state.transcriptLineCount(), 1u);
  EXPECT_EQ(state.transcriptLines()[0].text, "Hello world!");
  EXPECT_EQ(state.transcriptLines()[0].kind, TranscriptLine::Kind::AssistantText);
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

  state.appendStreamingDelta("streaming");
  EXPECT_EQ(state.activityContext(), ActivityContext::Streaming);

  state.finalizeStreamingLine();
  state.setAgentStatus(firmius::shared::AgentStatus::Idle);
  EXPECT_EQ(state.activityContext(), ActivityContext::Idle);

  PendingPermission perm;
  perm.title = "test";
  state.setPendingPermission(perm);
  EXPECT_EQ(state.activityContext(), ActivityContext::PermissionPending);
}
