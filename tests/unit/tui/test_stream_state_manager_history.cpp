#include "StreamStateManager.hpp"
#include "Message.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::shared::AgentHistory;
using firmius::shared::AgentTurn;
using firmius::shared::AgentProcessOutput;
using firmius::shared::AgentProcessSpawned;
using firmius::shared::Message;
using firmius::shared::Role;
using firmius::shared::TextContent;
using firmius::shared::ToolCallContent;
using firmius::shared::ToolResultContent;
using firmius::tui::StreamStateManager;
using firmius::shared::ToolPhase;

TEST(StreamStateManagerHistoryTest, RebuildFindsToolResultsRegardlessOfMessageRole) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      TextContent{"working"},
      ToolCallContent{"tool-1", "file_read", R"({"path":"src/main.rs"})"},
      ToolResultContent{"tool-1", R"({"content":"fn main() {}"})", true, "", ""},
  };
  turn.messages.push_back(assistant);
  history.turns.push_back(turn);

  StreamStateManager state;
  state.rebuildToolCallsFromHistory("agent-1", &history, "thread-1", false);

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::Finished);
  EXPECT_TRUE(view->success);
}

TEST(StreamStateManagerHistoryTest,
     TimeoutProcessMovesToBackgroundAndFinishesLater) {
  StreamStateManager state;

  state.handleAgentToolCall(
      {"agent-1", "tool-1", "process_execute", R"({"command":"sleep 5"})", ""});
  state.handleAgentProcessSpawned(
      AgentProcessSpawned{"agent-1", "proc-1", "tool-1", "sleep 5", ""});

  AgentTurn turn;
  turn.turnId = "turn-1";
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "tool-1",
      R"({"stdout":"","stderr":"","exit_code":-1,"duration_ms":15000,"finish_reason":"Timeout","process_id":"proc-1"})",
      true,
      "proc-1",
      ""}};
  turn.messages.push_back(tool_result);

  state.handleAgentTurnCompleted({"agent-1", turn, {}, ""});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::BackgroundRunning);

  auto counts = state.getProcessCounts("agent-1");
  EXPECT_EQ(counts.live, 0);
  EXPECT_EQ(counts.background, 1);

  state.handleAgentProcessOutput(
      AgentProcessOutput{"agent-1", "proc-1", "tick\n", false, false, -1, 0.0, ""});
  EXPECT_EQ(view->live_process_output, "tick\n");
  EXPECT_EQ(view->phase, ToolPhase::BackgroundRunning);

  state.handleAgentProcessOutput(
      AgentProcessOutput{"agent-1", "proc-1", "", false, true, 0, 18000.0, ""});
  EXPECT_EQ(view->phase, ToolPhase::Finished);
  EXPECT_TRUE(view->success);
  EXPECT_TRUE(view->process_exit_known);
  EXPECT_EQ(view->process_exit_code, 0);

  counts = state.getProcessCounts("agent-1");
  EXPECT_EQ(counts.live, 0);
  EXPECT_EQ(counts.background, 0);
}

} // namespace
