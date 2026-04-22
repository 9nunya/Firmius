#include "StreamStateManager.hpp"
#include "ConfigLoader.hpp"
#include "components/ChatWindow.hpp"

#include <algorithm>
#include <gtest/gtest.h>

namespace {

using firmius::shared::AgentText;
using firmius::shared::AgentThinking;
using firmius::shared::AgentInterrupted;
using firmius::shared::AgentCompacting;
using firmius::shared::ContextCompacted;
using firmius::shared::AgentError;
using firmius::shared::AgentFinished;
using firmius::shared::AgentOutcome;
using firmius::shared::AgentSpawned;
using firmius::shared::AgentAccountSwitched;
using firmius::shared::AgentProviderWaiting;
using firmius::shared::AgentRetrying;
using firmius::shared::AgentToolCall;
using firmius::shared::AgentToolCallChunk;
using firmius::shared::AgentFileEdited;
using firmius::shared::AgentTurn;
using firmius::shared::AgentTurnCompleted;
using firmius::shared::Message;
using firmius::shared::Role;
using firmius::shared::ToolResultContent;
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

TEST(StreamStateManagerLiveTest, LeadingBlankLinesAreTrimmedFromFirstLiveChunks) {
  StreamStateManager state;

  state.handleAgentText(AgentText{"agent-1", "\n\nHello", ""});
  state.handleAgentText(AgentText{"agent-1", "\nworld", ""});
  state.handleAgentThinking(AgentThinking{"agent-1", "\n\nThinking", ""});
  state.handleAgentThinking(AgentThinking{"agent-1", "\nmore", ""});

  auto stream = state.getStream("agent-1");
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->text, "Hello\nworld");
  EXPECT_EQ(stream->thinking, "Thinking\nmore");

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 2u);
  EXPECT_EQ(timeline[0].message, "Hello\nworld");
  EXPECT_EQ(timeline[1].message, "Thinking\nmore");
}

TEST(StreamStateManagerLiveTest, ProseBeforeAndAfterToolAreDistinctTimelineSegments) {
  StreamStateManager state;

  state.handleAgentText(AgentText{"agent-1", "Before tool.", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "Files",
                    R"({"action":"Read","path":"src/main.cpp"})", ""});
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
      AgentToolCall{"agent-1", "tool-1", "Files",
                    R"({"action":"Read","path":"src/main.cpp"})", ""});
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
      AgentToolCall{"agent-1", "tool-1", "Files",
                    R"({"action":"Read","path":"src/main.cpp"})", ""});
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
      AgentToolCall{"agent-1", "tool-1", "Process",
                    R"({"action":"Execute","command":"echo hi"})", ""});

  AgentTurn turn;
  turn.turnId = "turn-1";
  state.handleAgentTurnCompleted(AgentTurnCompleted{"agent-1", turn, {}, ""});

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 1u);
  EXPECT_EQ(timeline[0].kind, TimelineEntry::Kind::ToolCall);
  EXPECT_EQ(timeline[0].id, "tool-1");
}

TEST(StreamStateManagerLiveTest,
     SuccessfulSingleFileEditRemainsVisibleAfterTurnCompletion) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-edit", "Edit",
                    R"({"path":"src/main.cpp","edits":[{"op":"replace_range"}]})",
                    ""});
  state.handleAgentFileEdited(AgentFileEdited{
      "agent-1", "", "src/main.cpp", "tool-edit",
      "@@ replace range @@\n-old value\n+new value\n", 1, 1});

  AgentTurn turn;
  turn.turnId = "turn-edit";
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {
      ToolResultContent{"tool-edit", R"({"path":"src/main.cpp"})", true, "", ""}};
  turn.messages.push_back(tool_result);

  state.handleAgentTurnCompleted(AgentTurnCompleted{"agent-1", turn, {}, ""});

  auto view = state.getToolView("tool-edit");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Finished);
  EXPECT_TRUE(view->success);
  EXPECT_EQ(view->result, R"({"path":"src/main.cpp"})");
  ASSERT_EQ(view->fileEditEvents.size(), 1u);
  EXPECT_EQ(view->fileEditEvents.front().path, "src/main.cpp");

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 1u);
  EXPECT_EQ(timeline.front().id, "tool-edit");
}

TEST(StreamStateManagerLiveTest,
     FileEditBecomesVisibleBeforeTurnCompletionAfterFirstFileMutation) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-edit", "Edit",
                    R"({"path":"src/main.cpp","edits":[{"op":"replace_range"}]})",
                    ""});
  state.handleAgentFileEdited(AgentFileEdited{
      "agent-1", "", "src/main.cpp", "tool-edit",
      "@@ replace range @@\n-old value\n+new value\n", 1, 1});

  auto view = state.getToolView("tool-edit");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Called);
  EXPECT_TRUE(view->success);
  ASSERT_EQ(view->fileEditEvents.size(), 1u);
  EXPECT_EQ(view->fileEditEvents.front().path, "src/main.cpp");

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 1u);
  EXPECT_EQ(timeline.front().id, "tool-edit");
}

TEST(StreamStateManagerLiveTest,
     SuccessfulMultiFileEditRemainsVisibleAfterTurnCompletion) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-edit", "Edit",
                    R"({"files":[{"path":"src/a.cpp"},{"path":"src/b.cpp"}]})",
                    ""});
  state.handleAgentFileEdited(AgentFileEdited{
      "agent-1", "", "src/a.cpp", "tool-edit",
      "@@ replace @@\n-old a\n+new a\n", 1, 1});
  state.handleAgentFileEdited(AgentFileEdited{
      "agent-1", "", "src/b.cpp", "tool-edit",
      "@@ insert @@\n+new b\n", 1, 0});

  AgentTurn turn;
  turn.turnId = "turn-edit";
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "tool-edit",
      R"({"files":[{"path":"src/a.cpp"},{"path":"src/b.cpp"}]})",
      true,
      "",
      ""}};
  turn.messages.push_back(tool_result);

  state.handleAgentTurnCompleted(AgentTurnCompleted{"agent-1", turn, {}, ""});

  auto view = state.getToolView("tool-edit");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Finished);
  EXPECT_TRUE(view->success);
  EXPECT_EQ(
      view->result,
      R"({"files":[{"path":"src/a.cpp"},{"path":"src/b.cpp"}]})");
  ASSERT_EQ(view->fileEditEvents.size(), 2u);
  EXPECT_EQ(view->fileEditEvents[0].path, "src/a.cpp");
  EXPECT_EQ(view->fileEditEvents[1].path, "src/b.cpp");

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 1u);
  EXPECT_EQ(timeline.front().id, "tool-edit");
}

TEST(StreamStateManagerLiveTest,
     MultiFileEditBecomesVisiblePerFileBeforeTurnCompletion) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-edit", "Edit",
                    R"({"files":[{"path":"src/a.cpp"},{"path":"src/b.cpp"}]})",
                    ""});
  state.handleAgentFileEdited(AgentFileEdited{
      "agent-1", "", "src/a.cpp", "tool-edit",
      "@@ replace @@\n-old a\n+new a\n", 1, 1});
  state.handleAgentFileEdited(AgentFileEdited{
      "agent-1", "", "src/b.cpp", "tool-edit",
      "@@ insert @@\n+new b\n", 1, 0});

  auto view = state.getToolView("tool-edit");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Called);
  ASSERT_EQ(view->fileEditEvents.size(), 2u);
  EXPECT_EQ(view->fileEditEvents[0].path, "src/a.cpp");
  EXPECT_EQ(view->fileEditEvents[1].path, "src/b.cpp");
}

TEST(StreamStateManagerLiveTest,
     MainAgentFileEditEventCreatesVisibleToolStateWithoutPriorToolCall) {
  StreamStateManager state;

  state.handleAgentFileEdited(AgentFileEdited{
      "agent-main", "", "src/only.cpp", "tool-edit", "", 0, 0});

  auto view = state.getToolView("tool-edit");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->agentId, "agent-main");
  EXPECT_EQ(view->name, "file_edit");
  EXPECT_EQ(view->phase, ToolPhase::Called);
  EXPECT_TRUE(view->success);
  ASSERT_EQ(view->fileEditEvents.size(), 1u);
  EXPECT_EQ(view->fileEditEvents.front().path, "src/only.cpp");

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 1u);
  EXPECT_EQ(timeline.front().kind, TimelineEntry::Kind::ToolCall);
  EXPECT_EQ(timeline.front().id, "tool-edit");
}

TEST(StreamStateManagerLiveTest,
     FileWriteResultUsesSameDurableVisibilityPathAfterTurnCompletion) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-write", "Edit",
                    R"({"path":"src/write.cpp","content":"hello\n"})", ""});
  state.handleAgentFileEdited(AgentFileEdited{
      "agent-1", "", "src/write.cpp", "tool-write",
      "@@ overwrite @@\n+hello\n", 1, 0});

  AgentTurn turn;
  turn.turnId = "turn-write";
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "tool-write",
      R"({"path":"src/write.cpp","added_lines":1,"removed_lines":0})",
      true,
      "",
      ""}};
  turn.messages.push_back(tool_result);

  state.handleAgentTurnCompleted(AgentTurnCompleted{"agent-1", turn, {}, ""});

  auto view = state.getToolView("tool-write");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Finished);
  EXPECT_TRUE(view->success);
  EXPECT_EQ(
      view->result,
      R"({"path":"src/write.cpp","added_lines":1,"removed_lines":0})");
  ASSERT_EQ(view->fileEditEvents.size(), 1u);
  EXPECT_EQ(view->fileEditEvents.front().path, "src/write.cpp");

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 1u);
  EXPECT_EQ(timeline.front().id, "tool-write");
}

TEST(StreamStateManagerLiveTest, ProseBreaksQuickToolClusters) {
  StreamStateManager state;

  state.handleAgentText(AgentText{"agent-1", "Intro", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "Files",
                    R"({"action":"Read","path":"a.cpp"})", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-2", "Files",
                    R"({"action":"Read","path":"b.cpp"})", ""});

  int cluster_one = state.getToolCallClusterId("tool-1");
  int cluster_two = state.getToolCallClusterId("tool-2");
  EXPECT_EQ(cluster_one, cluster_two);

  state.handleAgentText(AgentText{"agent-1", "More prose", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-3", "Files",
                    R"({"action":"Read","path":"c.cpp"})", ""});

  int cluster_three = state.getToolCallClusterId("tool-3");
  EXPECT_GT(cluster_three, cluster_one);
}

TEST(StreamStateManagerLiveTest, RetryingMarksInFlightToolCallsAsCancelled) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "Files",
                  R"({"action":"Read","path":"src/main.cpp"})", ""});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Called);

  state.handleAgentRetrying(
      AgentRetrying{"agent-1", 1, 5, 429, 1000, "retry", "", "acct", ""});

  view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Error);
  EXPECT_FALSE(view->success);
  EXPECT_NE(view->result.find("Retrying request"), std::string::npos);
}

TEST(StreamStateManagerLiveTest, RetryingClearsStuckPreparingToolCalls) {
  StreamStateManager state;

  state.handleAgentToolCallChunk(
      AgentToolCallChunk{0, "agent-1", "tool-prep", "Files", "", ""});

  auto view = state.getToolView("tool-prep");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Preparing);

  state.handleAgentRetrying(
      AgentRetrying{"agent-1", 1, 5, 429, 1000, "retry", "", "acct", ""});

  // Preparing-only tool calls should be cleared on retry.
  EXPECT_FALSE(static_cast<bool>(state.getToolView("tool-prep")));
}

TEST(StreamStateManagerLiveTest, WhitespaceOnlyProseDoesNotBreakQuickToolClusters) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "Files",
                    R"({"action":"Read","path":"a.cpp"})", ""});
  state.handleAgentText(AgentText{"agent-1", "\n\n   \t", ""});
  state.handleAgentThinking(AgentThinking{"agent-1", "\n\r\n", ""});
  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-2", "Files",
                    R"({"action":"Read","path":"b.cpp"})", ""});

  EXPECT_EQ(state.getToolCallClusterId("tool-1"),
            state.getToolCallClusterId("tool-2"));

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 2u);
  EXPECT_EQ(timeline[0].id, "tool-1");
  EXPECT_EQ(timeline[1].id, "tool-2");
}

TEST(StreamStateManagerLiveTest,
     MissingToolResultDoesNotForceFinishedState) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "Process",
                    R"({"action":"Execute","command":"sleep 5"})", ""});

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
  EXPECT_EQ(view->phase, ToolPhase::Preparing);
  EXPECT_EQ(view->args, R"({"path":"src/main.cpp"})");
  EXPECT_TRUE(view->name.empty());
  EXPECT_FALSE(firmius::shared::ToolCallHasRenderableIdentity(*view));
  EXPECT_FALSE(firmius::tui::ShouldRenderToolCallView(*view));

  state.handleAgentToolCallChunk(
      AgentToolCallChunk{0, "agent-1", "tool-1", "Files", "", ""});

  view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Preparing);
  EXPECT_EQ(view->name, "Files");
  EXPECT_EQ(view->args, R"({"path":"src/main.cpp"})");
  EXPECT_TRUE(firmius::shared::ToolCallHasRenderableIdentity(*view));
  EXPECT_TRUE(firmius::tui::ShouldRenderToolCallView(*view));

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "Files",
                    R"({"action":"Read","path":"src/main.cpp"})", ""});

  view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Called);
}

TEST(StreamStateManagerLiveTest, InterruptClearsProviderWaitingAndRetryUiImmediately) {
  StreamStateManager state;

  state.handleAgentProviderWaiting(AgentProviderWaiting{"agent-1", ""});
  state.handleAgentRetrying(
      AgentRetrying{"agent-1", 2, 5, 429, 4000, "Retrying request", "", "acct",
                    ""});
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

TEST(StreamStateManagerLiveTest, InterruptClearsTransientLiveProseRows) {
  StreamStateManager state;

  state.handleAgentThinking(AgentThinking{"agent-1", "thinking", ""});
  state.handleAgentText(AgentText{"agent-1", "partial response", ""});
  ASSERT_EQ(state.getTimeline().size(), 2u);

  state.handleAgentInterrupted(AgentInterrupted{"agent-1", ""});

  const auto *stream = state.getStream("agent-1");
  ASSERT_NE(stream, nullptr);
  EXPECT_TRUE(stream->thinking.empty());
  EXPECT_TRUE(stream->text.empty());
  EXPECT_TRUE(state.getTimeline().empty());
}

TEST(StreamStateManagerLiveTest,
     InterruptRemovesPreparingOnlyToolBlocks) {
  StreamStateManager state;

  state.handleAgentToolCallChunk(
      AgentToolCallChunk{0, "agent-1", "tool-prep", "Files", "", ""});

  auto view = state.getToolView("tool-prep");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Preparing);

  state.handleAgentInterrupted(AgentInterrupted{"agent-1", ""});

  EXPECT_FALSE(static_cast<bool>(state.getToolView("tool-prep")));
  EXPECT_TRUE(state.getTimeline().empty());
}

TEST(StreamStateManagerLiveTest,
     InterruptMarksRunningToolBlocksAsAborted) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"agent-1", "tool-1", "Files",
                    R"({"action":"Read","path":"src/main.cpp"})", ""});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Called);

  state.handleAgentInterrupted(AgentInterrupted{"agent-1", ""});

  view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Error);
  EXPECT_FALSE(view->success);
  EXPECT_EQ(view->result, "User aborted tool manually.");
}

TEST(StreamStateManagerLiveTest, ContextCompactedClearsTransientCompactionState) {
  StreamStateManager state;
  state.handleAgentCompacting(AgentCompacting{"agent-1", ""});
  state.handleAgentCompactionThinking(
      firmius::shared::AgentCompactionThinking{"agent-1", "thinking", ""});
  state.handleAgentCompactionText(
      firmius::shared::AgentCompactionText{"agent-1", "summary", ""});
  state.handleContextCompacted(ContextCompacted{"agent-1", 42, ""});

  auto stream = state.getStream("agent-1");
  ASSERT_NE(stream, nullptr);
  EXPECT_FALSE(stream->compaction_active);
  EXPECT_FALSE(stream->compaction_finished);
  EXPECT_TRUE(stream->compaction_thinking.empty());
  EXPECT_TRUE(stream->compaction_text.empty());
  EXPECT_TRUE(stream->compaction_completion.empty());
}

TEST(StreamStateManagerLiveTest,
     NewLiveOutputAfterCompactionDoesNotResurrectTransientCompactionState) {
  StreamStateManager state;
  state.handleAgentCompacting(AgentCompacting{"agent-1", ""});
  state.handleAgentCompactionText(
      firmius::shared::AgentCompactionText{"agent-1", "summary", ""});
  state.handleContextCompacted(ContextCompacted{"agent-1", 42, ""});

  state.handleAgentText(AgentText{"agent-1", "next turn output", ""});

  auto stream = state.getStream("agent-1");
  ASSERT_NE(stream, nullptr);
  EXPECT_FALSE(stream->compaction_active);
  EXPECT_FALSE(stream->compaction_finished);
  EXPECT_TRUE(stream->compaction_thinking.empty());
  EXPECT_TRUE(stream->compaction_text.empty());
  EXPECT_TRUE(stream->compaction_completion.empty());
  EXPECT_EQ(stream->text, "next turn output");
}

TEST(StreamStateManagerLiveTest, ChildErrorMarksParentSubagentAsFailed) {
  StreamStateManager state;
  state.handleAgentToolCall(
      AgentToolCall{"parent", "tool-1", "Delegate",
                    R"({"name":"child","title":"Child"})", ""});

  state.handleAgentSpawned(
      AgentSpawned{"child-id", "worker", "parent", "child", "Child", true, "", "", 0},
      "parent");
  state.handleAgentError(AgentError{"child-id", "boom", "parent"});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Error);
  EXPECT_FALSE(view->subagent_running);
}

TEST(StreamStateManagerLiveTest, ChildErrorWithEmptyParentLogRecordsFailureActivity) {
  StreamStateManager state;
  state.handleAgentToolCall(
      AgentToolCall{"parent", "tool-1", "Delegate",
                    R"({"name":"child","title":"Child"})", ""});

  state.handleAgentSpawned(
      AgentSpawned{"child-id", "worker", "parent", "child", "Child", true, "", "", 0},
      "parent");

  const auto *subagentBeforeError = state.getSubagentStateForToolCall("tool-1");
  ASSERT_NE(subagentBeforeError, nullptr);
  const size_t activityCountBeforeError =
      subagentBeforeError->activity_log.size();

  state.handleAgentError(AgentError{"child-id", "boom", "parent"});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  ASSERT_FALSE(view->subagent_tool_log.empty());
  EXPECT_EQ(view->subagent_tool_log.back().summary, "Failed: boom");
  EXPECT_EQ(view->phase, ToolPhase::Error);

  const auto *subagentAfterError = state.getSubagentStateForToolCall("tool-1");
  ASSERT_NE(subagentAfterError, nullptr);
  ASSERT_GE(subagentAfterError->activity_log.size(),
            activityCountBeforeError + 1);
  EXPECT_EQ(subagentAfterError->activity_log.back().summary, "Failed: boom");
  EXPECT_EQ(subagentAfterError->outcome,
            firmius::tui::SubagentOutcomeKind::Failed);
}

TEST(StreamStateManagerLiveTest,
     RateLimitRetryWithRawBodyAppendsLiveErrorTimelineEntry) {
  StreamStateManager state;

  state.handleAgentRetrying(
      AgentRetrying{
          "agent-1",
          1,
          5,
          429,
          0,
          "rate limited, switching account",
          "",
          "Key #1",
          "Quota exhausted or rate limited. (HTTP 429)\n"
          "Provider: zen\n"
          "Model: opencode/test\n"
          "Raw provider body:\n"
          R"({"error":{"message":"rate limit reached","type":"rate_limit"}})"});

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 1u);
  EXPECT_EQ(timeline[0].kind, TimelineEntry::Kind::Error);
  EXPECT_EQ(timeline[0].agentId, "agent-1");
  EXPECT_NE(timeline[0].message.find("Raw provider body:"),
            std::string::npos);
}

TEST(StreamStateManagerLiveTest,
     RateLimitProviderErrorWithRawBodyAppendsLiveErrorTimelineEntry) {
  StreamStateManager state;
  auto config = firmius::shared::ConfigLoader::instance().getConfig();
  config.hideErrors = false;
  firmius::shared::ConfigLoader::instance().updateConfig(config);

  state.handleAgentError(AgentError{
      "agent-1",
      "Provider stream error: Quota exhausted or rate limited. Switching to next account... (HTTP 429)\n"
      "Provider: zen\n"
      "Model: opencode/test\n"
      "Raw provider body:\n"
      R"({"error":{"message":"rate limit reached","type":"rate_limit"}})",
      ""});

  const auto &timeline = state.getTimeline();
  ASSERT_EQ(timeline.size(), 1u);
  EXPECT_EQ(timeline[0].kind, TimelineEntry::Kind::Error);
  EXPECT_EQ(timeline[0].agentId, "agent-1");
  EXPECT_NE(timeline[0].message.find("Raw provider body:"),
            std::string::npos);
}

TEST(StreamStateManagerLiveTest,
     GenericErrorDoesNotAppendLiveErrorTimelineEntry) {
  StreamStateManager state;

  state.handleAgentError(AgentError{"agent-1", "boom", ""});

  EXPECT_TRUE(state.getTimeline().empty());
}

TEST(StreamStateManagerLiveTest,
     RetryingSubagentReactivatesParentSummonBlockAndCanFinishSuccessfully) {
  StreamStateManager state;
  state.handleAgentToolCall(
      AgentToolCall{"parent", "tool-1", "Delegate",
                    R"({"name":"child","title":"Child"})", ""});
  state.handleAgentSpawned(
      AgentSpawned{"child-id", "worker", "parent", "child", "Child", true, "", "", 0},
      "parent");
  state.handleAgentError(AgentError{"child-id", "boom", "parent"});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Error);
  EXPECT_FALSE(view->subagent_running);

  state.handleAgentRetrying(
      AgentRetrying{"child-id", 2, 5, 429, 4000, "Retrying request",
                    "parent", "acct", ""});

  view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Called);
  EXPECT_TRUE(view->subagent_running);

  state.handleAgentFinished(AgentFinished{
      "child-id", AgentOutcome{AgentOutcome::Kind::Response, "All set"},
      "parent"});

  view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Finished);
  EXPECT_FALSE(view->subagent_running);
  ASSERT_FALSE(view->subagent_tool_log.empty());
  EXPECT_EQ(view->subagent_tool_log.back().summary, "Done");
  EXPECT_TRUE(std::any_of(view->subagent_tool_log.begin(),
                          view->subagent_tool_log.end(),
                          [](const firmius::shared::SubagentToolLogEntry &entry) {
                            return entry.summary.find("Failed: boom") !=
                                   std::string::npos;
                          }));
}

TEST(StreamStateManagerLiveTest,
     RetriedSubagentStillEndsInErrorWhenFinalAttemptFails) {
  StreamStateManager state;
  state.handleAgentToolCall(
      AgentToolCall{"parent", "tool-1", "Delegate",
                    R"({"name":"child","title":"Child"})", ""});
  state.handleAgentSpawned(
      AgentSpawned{"child-id", "worker", "parent", "child", "Child", true, "", "", 0},
      "parent");
  state.handleAgentError(AgentError{"child-id", "boom", "parent"});
  state.handleAgentRetrying(
      AgentRetrying{"child-id", 2, 5, 429, 4000, "Retrying request",
                    "parent", "acct", ""});

  state.handleAgentFinished(AgentFinished{
      "child-id", AgentOutcome{AgentOutcome::Kind::Failed, "final boom"},
      "parent"});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Error);
  EXPECT_FALSE(view->subagent_running);
  ASSERT_FALSE(view->subagent_tool_log.empty());
  EXPECT_EQ(view->subagent_tool_log.back().summary, "Failed: final boom");
}

TEST(StreamStateManagerLiveTest, FinishedSubagentNoSummaryUsesTypedOutcome) {
  StreamStateManager state;
  state.handleAgentToolCall(
      AgentToolCall{"parent", "tool-1", "Delegate",
                    R"({"agent_id":"child-id"})", ""});
  state.handleAgentSpawned(
      AgentSpawned{"child-id", "worker", "parent", "child", "Child", true, "", "", 0},
      "parent");
  state.handleAgentFinished(AgentFinished{
      "child-id", AgentOutcome{AgentOutcome::Kind::NoSummary, ""},
      "parent"});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Finished);
  EXPECT_FALSE(view->subagent_running);
  ASSERT_FALSE(view->subagent_tool_log.empty());
  EXPECT_EQ(view->subagent_tool_log.back().summary, "Done (no summary)");
}

TEST(StreamStateManagerLiveTest, FinishedSubagentResponseCancelAndFailureRenderCorrectly) {
  StreamStateManager state;

  state.handleAgentToolCall(
      AgentToolCall{"parent", "tool-1", "Delegate",
                    R"({"agent_id":"child-response"})", ""});
  state.handleAgentSpawned(
      AgentSpawned{"child-response", "worker", "parent", "child", "Child", true, "", "", 0},
      "parent");
  state.handleAgentFinished(AgentFinished{
      "child-response", AgentOutcome{AgentOutcome::Kind::Response, "All set"},
      "parent"});
  auto responseView = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(responseView));
  ASSERT_FALSE(responseView->subagent_tool_log.empty());
  EXPECT_EQ(responseView->subagent_tool_log.back().summary, "Done");

  state.handleAgentToolCall(
      AgentToolCall{"parent", "tool-2", "Delegate",
                    R"({"agent_id":"child-cancel"})", ""});
  state.handleAgentSpawned(
      AgentSpawned{"child-cancel", "worker", "parent", "child", "Child", true, "", "", 0},
      "parent");
  state.handleAgentFinished(AgentFinished{
      "child-cancel", AgentOutcome{AgentOutcome::Kind::Cancelled, "Stopped"},
      "parent"});
  auto cancelledView = state.getToolView("tool-2");
  ASSERT_TRUE(static_cast<bool>(cancelledView));
  ASSERT_FALSE(cancelledView->subagent_tool_log.empty());
  EXPECT_EQ(cancelledView->subagent_tool_log.back().summary, "Cancelled");
  EXPECT_EQ(cancelledView->phase, ToolPhase::Finished);

  state.handleAgentToolCall(
      AgentToolCall{"parent", "tool-3", "Delegate",
                    R"({"agent_id":"child-fail"})", ""});
  state.handleAgentSpawned(
      AgentSpawned{"child-fail", "worker", "parent", "child", "Child", true, "", "", 0},
      "parent");
  state.handleAgentFinished(AgentFinished{
      "child-fail", AgentOutcome{AgentOutcome::Kind::Failed, "boom"},
      "parent"});
  auto failedView = state.getToolView("tool-3");
  ASSERT_TRUE(static_cast<bool>(failedView));
  ASSERT_FALSE(failedView->subagent_tool_log.empty());
  EXPECT_EQ(failedView->subagent_tool_log.back().summary, "Failed: boom");
  EXPECT_EQ(failedView->phase, ToolPhase::Error);
}

TEST(StreamStateManagerLiveTest,
     SubagentNormalizedStateTracksLifecycleRoutingAndArtifacts) {
  StreamStateManager state;

  state.handleAgentToolCall(AgentToolCall{
      "parent", "summon-1", "Delegate",
      R"({"name":"worker","title":"Worker","task":"Implement API","category":"executor"})",
      ""});
  state.handleAgentSpawned(AgentSpawned{"child-1", "worker", "parent", "worker",
                                        "Worker", true, "", "", 0},
                           "parent");

  const auto *spawned = state.getSubagentStateForToolCall("summon-1");
  ASSERT_NE(spawned, nullptr);
  EXPECT_EQ(spawned->child_agent_id, "child-1");
  EXPECT_EQ(spawned->child_title, "Worker");
  EXPECT_TRUE(spawned->running);
  EXPECT_EQ(spawned->task, "Implement API");

  state.handleAgentProviderWaiting(AgentProviderWaiting{"child-1", "parent"});
  auto *waiting = state.getSubagentStateForToolCall("summon-1");
  ASSERT_NE(waiting, nullptr);
  EXPECT_TRUE(waiting->provider_waiting);
  EXPECT_EQ(waiting->wait_state, "provider_waiting");

  state.handleAgentRetrying(
      AgentRetrying{"child-1", 2, 4, 429, 2000, "retry", "parent", "acct-a",
                    ""});
  auto *retrying = state.getSubagentStateForToolCall("summon-1");
  ASSERT_NE(retrying, nullptr);
  EXPECT_TRUE(retrying->retrying);
  EXPECT_EQ(retrying->wait_state, "retrying");

  state.handleAgentAccountSwitched(
      AgentAccountSwitched{"child-1", "acct-b", "parent"});
  auto *switched = state.getSubagentStateForToolCall("summon-1");
  ASSERT_NE(switched, nullptr);
  EXPECT_TRUE(switched->account_switched);
  EXPECT_EQ(switched->wait_state, "account_switched");

  AgentTurn turn;
  turn.turnId = "turn-1";
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "summon-1",
      R"({"agentId":"child-1","status":"completed","result":"done","fallback_used":true,"category":"scout","attempted_categories":["executor","scout"],"artifacts_created":[{"reference":"@artifact:worker/report.md"}],"artifacts_updated":[{"reference":"@artifact:worker/index.json"}]})",
      true,
      "",
      ""}};
  turn.messages.push_back(tool_result);
  state.handleAgentTurnCompleted(AgentTurnCompleted{"parent", turn, {}, ""});

  const auto *completed = state.getSubagentStateForToolCall("summon-1");
  ASSERT_NE(completed, nullptr);
  EXPECT_EQ(completed->wait_state, "completed");
  EXPECT_TRUE(completed->fallback_used);
  EXPECT_EQ(completed->route_category, "scout");
  ASSERT_EQ(completed->attempted_categories.size(), 2u);
  EXPECT_EQ(completed->attempted_categories[0], "executor");
  EXPECT_EQ(completed->attempted_categories[1], "scout");
  ASSERT_EQ(completed->artifacts_created.size(), 1u);
  ASSERT_EQ(completed->artifacts_updated.size(), 1u);
  EXPECT_NE(completed->artifacts_created[0].find("@artifact:worker/report.md"),
            std::string::npos);
  EXPECT_NE(completed->artifacts_updated[0].find("@artifact:worker/index.json"),
            std::string::npos);
}

} // namespace
