#include "StreamStateManager.hpp"
#include "components/ChatWindow.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::shared::AgentText;
using firmius::shared::AgentThinking;
using firmius::shared::AgentInterrupted;
using firmius::shared::AgentAccountSwitched;
using firmius::shared::AgentProviderWaiting;
using firmius::shared::AgentRetrying;
using firmius::shared::AgentToolCall;
using firmius::shared::AgentToolCallChunk;
using firmius::shared::AgentTurn;
using firmius::shared::AgentTurnCompleted;
using firmius::shared::ToolPhase;
using firmius::tui::TimelineEntry;
using firmius::tui::StreamStateManager;

TEST(StreamStateManagerLiveTest, LiveProseAndThinkingBecomeTimelineEntries) {
  StreamStateManager state;

  state.handleAgentText(AgentText{"agent-1", "Hello ", ""});
  state.handleAgentText(AgentText{"agent-1", "world", ""});
  state.handleAgentThinking(AgentThinking{"agent-1", "thinking", ""});

  auto stream = state.getStream("agent-1");
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->text, "Hello world");
  EXPECT_EQ(stream->thinking, "thinking");

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 2u);
  EXPECT_EQ(timeline[0].kind, TimelineEntry::Kind::Text);
  EXPECT_EQ(timeline[0].agentId, "agent-1");
  EXPECT_EQ(timeline[0].message, "Hello world");
  EXPECT_EQ(timeline[1].kind, TimelineEntry::Kind::Thinking);
  EXPECT_EQ(timeline[1].message, "thinking");
}

TEST(StreamStateManagerLiveTest, ProseBeforeAndAfterToolAreDistinctTimelineSegments) {
  StreamStateManager state;

  state.handleAgentText(AgentText{"agent-1", "Before tool.", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "file_read",
                    R"({"path":"src/main.cpp"})", ""});
  state.handleAgentText(AgentText{"agent-1", "After tool.", ""});

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 3u);
  EXPECT_EQ(timeline[0].kind, TimelineEntry::Kind::Text);
  EXPECT_EQ(timeline[0].message, "Before tool.");
  EXPECT_EQ(timeline[1].kind, TimelineEntry::Kind::ToolCall);
  EXPECT_EQ(timeline[1].id, "tool-1");
  EXPECT_EQ(timeline[2].kind, TimelineEntry::Kind::Text);
  EXPECT_EQ(timeline[2].message, "After tool.");
}

TEST(StreamStateManagerLiveTest, ProseDeltasAfterToolAppendToNewEpisode) {
  StreamStateManager state;

  state.handleAgentText(AgentText{"agent-1", "Intro.", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "file_read",
                    R"({"path":"src/main.cpp"})", ""});
  state.handleAgentText(AgentText{"agent-1", "Post ", ""});
  state.handleAgentText(AgentText{"agent-1", "tool prose.", ""});

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 3u);
  EXPECT_EQ(timeline[0].kind, TimelineEntry::Kind::Text);
  EXPECT_EQ(timeline[1].kind, TimelineEntry::Kind::ToolCall);
  EXPECT_EQ(timeline[2].kind, TimelineEntry::Kind::Text);
  EXPECT_EQ(timeline[2].message, "Post tool prose.");
}

TEST(StreamStateManagerLiveTest, ThinkingEpisodeResetsAcrossToolBoundary) {
  StreamStateManager state;

  state.handleAgentThinking(AgentThinking{"agent-1", "Think A ", ""});
  state.handleAgentThinking(AgentThinking{"agent-1", "Think B", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "file_read",
                    R"({"path":"src/main.cpp"})", ""});
  state.handleAgentThinking(AgentThinking{"agent-1", "Think C", ""});

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 3u);
  EXPECT_EQ(timeline[0].kind, TimelineEntry::Kind::Thinking);
  EXPECT_EQ(timeline[0].message, "Think A Think B");
  EXPECT_EQ(timeline[1].kind, TimelineEntry::Kind::ToolCall);
  EXPECT_EQ(timeline[2].kind, TimelineEntry::Kind::Thinking);
  EXPECT_EQ(timeline[2].message, "Think C");
}

TEST(StreamStateManagerLiveTest, TurnCompletedClearsTransientProseTimelineRows) {
  StreamStateManager state;

  state.handleAgentText(AgentText{"agent-1", "draft text", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "process_execute",
                    R"({"command":"echo hi"})", ""});

  AgentTurn turn;
  turn.turnId = "turn-1";
  state.handleAgentTurnCompleted(AgentTurnCompleted{"agent-1", turn, {}, ""});

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 1u);
  EXPECT_EQ(timeline[0].kind, TimelineEntry::Kind::ToolCall);
  EXPECT_EQ(timeline[0].id, "tool-1");
}

TEST(StreamStateManagerLiveTest, ProseBreaksQuickToolClusters) {
  StreamStateManager state;

  state.handleAgentText(AgentText{"agent-1", "Intro", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "file_read",
                    R"({"path":"a.cpp"})", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-2", "file_read",
                    R"({"path":"b.cpp"})", ""});

  int cluster_one = state.getToolCallClusterId("tool-1");
  int cluster_two = state.getToolCallClusterId("tool-2");
  EXPECT_EQ(cluster_one, cluster_two);

  state.handleAgentText(AgentText{"agent-1", "More prose", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-3", "file_read",
                    R"({"path":"c.cpp"})", ""});

  int cluster_three = state.getToolCallClusterId("tool-3");
  EXPECT_GT(cluster_three, cluster_one);
}

TEST(StreamStateManagerLiveTest,
     MissingToolResultDoesNotForceFinishedState) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "process_execute",
                    R"({"command":"sleep 5"})", ""});

  AgentTurn turn;
  turn.turnId = "turn-1";
  state.handleAgentTurnCompleted(
      AgentTurnCompleted{"agent-1", turn, {}, ""});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Called);
  EXPECT_FALSE(view->success);
}

TEST(StreamStateManagerLiveTest, ArgsBeforeNameChunkDoesNotRenderUntilNameArrives) {
  StreamStateManager state;

  state.handleAgentToolCallChunk(
      AgentToolCallChunk{0, "agent-1", "tool-1", "",
                         R"({"path":"src/main.cpp"})", ""});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Called);
  EXPECT_EQ(view->args, R"({"path":"src/main.cpp"})");
  EXPECT_TRUE(view->name.empty());
  EXPECT_FALSE(firmius::shared::ToolCallHasRenderableIdentity(*view));
  EXPECT_FALSE(firmius::tui::ShouldRenderToolCallView(*view));

  state.handleAgentToolCallChunk(
      AgentToolCallChunk{0, "agent-1", "tool-1", "file_read", "", ""});

  view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Called);
  EXPECT_EQ(view->name, "file_read");
  EXPECT_EQ(view->args, R"({"path":"src/main.cpp"})");
  EXPECT_TRUE(firmius::shared::ToolCallHasRenderableIdentity(*view));
  EXPECT_TRUE(firmius::tui::ShouldRenderToolCallView(*view));
}

TEST(StreamStateManagerLiveTest, InterruptClearsProviderWaitingAndRetryUiImmediately) {
  StreamStateManager state;

  state.handleAgentProviderWaiting(AgentProviderWaiting{"agent-1", ""});
  state.handleAgentRetrying(
      AgentRetrying{"agent-1", 2, 5, 429, 4000, "Retrying request", "", "acct"});
  state.handleAgentAccountSwitched(AgentAccountSwitched{"agent-1", "acct", ""});

  auto stream = state.getStream("agent-1");
  ASSERT_NE(stream, nullptr);
  EXPECT_TRUE(stream->provider_waiting);
  EXPECT_FALSE(state.getRetryStatus().empty());
  ASSERT_EQ(state.getAccountSwaps().size(), 1u);

  state.handleAgentInterrupted(AgentInterrupted{"agent-1", ""});

  stream = state.getStream("agent-1");
  ASSERT_NE(stream, nullptr);
  EXPECT_FALSE(stream->provider_waiting);
  EXPECT_TRUE(state.getRetryStatus().empty());
  EXPECT_TRUE(state.getAccountSwaps().empty());
}

} // namespace
