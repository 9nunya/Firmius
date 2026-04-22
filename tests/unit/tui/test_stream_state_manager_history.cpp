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
using firmius::tui::ProcessRuntimeSnapshot;
using firmius::shared::ToolPhase;

TEST(StreamStateManagerHistoryTest, RebuildFindsToolResultsRegardlessOfMessageRole) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      TextContent{"working"},
      ToolCallContent{"tool-1", "Files", R"({"action":"Read","path":"src/main.rs"})"},
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
     RebuildPreservesFileEditFallbackSignalsFromHistory) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-file-edit";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {ToolCallContent{
      "tool-edit", "Edit", R"({"path":"src/main.cpp"})"}};

  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "tool-edit",
      R"({"path":"src/main.cpp","added_lines":3,"removed_lines":1,"diff_preview":"@@ replace @@\n-old\n+new\n"})",
      true,
      "",
      ""}};

  turn.messages.push_back(std::move(assistant));
  turn.messages.push_back(std::move(tool_result));
  history.turns.push_back(std::move(turn));

  StreamStateManager state;
  state.rebuildToolCallsFromHistory("agent-1", &history, "thread-1", false);

  auto view = state.getToolView("tool-edit");
  ASSERT_TRUE(static_cast<bool>(view));
  ASSERT_EQ(view->fileEditEvents.size(), 1u);
  EXPECT_EQ(view->fileEditEvents.front().path, "src/main.cpp");
  EXPECT_EQ(view->fileEditEvents.front().addedLines, 3);
  EXPECT_EQ(view->fileEditEvents.front().removedLines, 1);
  EXPECT_NE(view->fileEditEvents.front().diffPreview.find("@@ replace @@"),
            std::string::npos);
}

TEST(StreamStateManagerHistoryTest,
     RebuildDerivesFileEditFallbackSignalsFromOperationsWhenPreviewMissing) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-file-edit-ops";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {ToolCallContent{
      "tool-edit", "Edit", R"({"path":"src/main.cpp","content":"hello\nworld\n"})"}};

  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "tool-edit",
      R"({"path":"src/main.cpp","mode":"overwrite","applied_edits":1,"added_lines":2,"removed_lines":0,"operations":[{"op":"overwrite_file_content","description":"create file","start_line":1,"end_line":2,"old_lines":[],"new_lines":["hello","world"]}]})",
      true,
      "",
      ""}};

  turn.messages.push_back(std::move(assistant));
  turn.messages.push_back(std::move(tool_result));
  history.turns.push_back(std::move(turn));

  StreamStateManager state;
  state.rebuildToolCallsFromHistory("agent-1", &history, "thread-1", false);

  auto view = state.getToolView("tool-edit");
  ASSERT_TRUE(static_cast<bool>(view));
  ASSERT_EQ(view->fileEditEvents.size(), 1u);
  EXPECT_EQ(view->fileEditEvents.front().path, "src/main.cpp");
  EXPECT_EQ(view->fileEditEvents.front().addedLines, 2);
  EXPECT_EQ(view->fileEditEvents.front().removedLines, 0);
  EXPECT_NE(view->fileEditEvents.front().diffPreview.find("@@ create file @@"),
            std::string::npos);
  EXPECT_NE(view->fileEditEvents.front().diffPreview.find("+hello"),
            std::string::npos);
}

TEST(StreamStateManagerHistoryTest,
     TimeoutProcessMovesToBackgroundAndFinishesLater) {
  StreamStateManager state;

  state.handleAgentToolCall(
      {"agent-1", "tool-1", "Process", R"({"action":"Execute","command":"sleep 5"})", ""});
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
  auto process = state.getProcessState("proc-1");
  ASSERT_NE(process, nullptr);
  EXPECT_TRUE(process->running);
  EXPECT_FALSE(process->finished);
  EXPECT_TRUE(process->is_background);

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
  process = state.getProcessState("proc-1");
  ASSERT_NE(process, nullptr);
  EXPECT_TRUE(process->finished);
  EXPECT_FALSE(process->running);
  EXPECT_TRUE(process->exit_code_known);
  EXPECT_EQ(process->exit_code, 0);
  EXPECT_EQ(static_cast<int>(process->duration_ms), 18000);

  counts = state.getProcessCounts("agent-1");
  EXPECT_EQ(counts.live, 0);
  EXPECT_EQ(counts.background, 0);
}

TEST(StreamStateManagerHistoryTest,
     BuffersEarlyOutputUntilSpawnAssociationArrives) {
  StreamStateManager state;

  state.handleAgentToolCall(
      {"agent-1", "tool-1", "Process", R"({"action":"Execute","command":"sleep 5"})", ""});
  state.handleAgentProcessOutput(
      AgentProcessOutput{"agent-1", "proc-1", "early\n", false, false, -1, 0.0, ""});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_TRUE(view->live_process_output.empty());

  state.handleAgentProcessSpawned(
      AgentProcessSpawned{"agent-1", "proc-1", "tool-1", "sleep 5", ""});

  EXPECT_EQ(view->live_process_output, "early\n");
  EXPECT_EQ(view->process_id, "proc-1");
  auto process = state.getProcessState("proc-1");
  ASSERT_NE(process, nullptr);
  EXPECT_EQ(process->latest_output_tail, "early\n");
  EXPECT_EQ(process->command, "sleep 5");
  EXPECT_EQ(process->origin_tool_call_id, "tool-1");
}

TEST(StreamStateManagerHistoryTest,
     ProcessSpawnStaysLiveAndAcceptsOutputUntilFinished) {
  StreamStateManager state;

  state.handleAgentToolCall(
      {"agent-1", "tool-1", "Process", R"({"action":"Spawn","command":"tail -f app.log"})", ""});
  state.handleAgentProcessSpawned(
      AgentProcessSpawned{"agent-1", "proc-1", "tool-1", "tail -f app.log", ""});

  AgentTurn turn;
  turn.turnId = "turn-1";
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "tool-1", R"({"action":"Status","process_id":"proc-1"})", true, "proc-1", ""}};
  turn.messages.push_back(tool_result);
  state.handleAgentTurnCompleted({"agent-1", turn, {}, ""});

  auto view = state.getToolView("tool-1");
  ASSERT_TRUE(static_cast<bool>(view));
  EXPECT_EQ(view->phase, ToolPhase::BackgroundRunning);

  state.handleAgentProcessOutput(
      AgentProcessOutput{"agent-1", "proc-1", "tick\n", false, false, -1, 0.0, ""});
  EXPECT_EQ(view->live_process_output, "tick\n");
  EXPECT_EQ(view->phase, ToolPhase::BackgroundRunning);

  state.handleAgentProcessOutput(
      AgentProcessOutput{"agent-1", "proc-1", "", false, true, 0, 2000.0, ""});
  EXPECT_EQ(view->phase, ToolPhase::Finished);
  EXPECT_TRUE(view->success);
  auto process = state.getProcessState("proc-1");
  ASSERT_NE(process, nullptr);
  EXPECT_TRUE(process->finished);
  EXPECT_EQ(process->exit_code, 0);
  EXPECT_EQ(static_cast<int>(process->duration_ms), 2000);
}

TEST(StreamStateManagerHistoryTest,
     ProcessCountsUseRuntimeSnapshotEvenWithoutTransientMaps) {
  StreamStateManager state;

  ProcessRuntimeSnapshot runtime;
  runtime.blocking_process_ids = {"live-1"};
  runtime.owned_process_ids = {"live-1", "bg-1"};

  auto counts = state.getProcessCounts(
      "agent-1", &runtime, [](const std::string &process_id) {
        return process_id == "live-1" || process_id == "bg-1";
      });

  EXPECT_EQ(counts.live, 1);
  EXPECT_EQ(counts.background, 1);
}

TEST(StreamStateManagerHistoryTest,
     SpawnedProcessEntersNormalizedStateImmediately) {
  StreamStateManager state;
  state.handleAgentToolCall(
      {"agent-1", "tool-1", "Process", R"({"action":"Spawn","command":"tail -f app.log"})", ""});
  state.handleAgentProcessSpawned(
      AgentProcessSpawned{"agent-1", "proc-1", "tool-1", "tail -f app.log", ""});

  auto process = state.getProcessState("proc-1");
  ASSERT_NE(process, nullptr);
  EXPECT_EQ(process->owner_agent_id, "agent-1");
  EXPECT_EQ(process->origin_tool_call_id, "tool-1");
  EXPECT_EQ(process->command, "tail -f app.log");
  EXPECT_TRUE(process->running);
  EXPECT_FALSE(process->finished);
}

TEST(StreamStateManagerHistoryTest,
     ProcessCountsAreDerivedFromNormalizedStateByOwnerAgent) {
  StreamStateManager state;
  state.handleAgentToolCall(
      {"agent-a", "tool-a", "Process", R"({"action":"Spawn","command":"tail -f a.log"})", ""});
  state.handleAgentProcessSpawned(
      AgentProcessSpawned{"agent-a", "proc-a", "tool-a", "tail -f a.log", ""});
  state.handleAgentToolCall(
      {"agent-b", "tool-b", "Process", R"({"action":"Spawn","command":"tail -f b.log"})", ""});
  state.handleAgentProcessSpawned(
      AgentProcessSpawned{"agent-b", "proc-b", "tool-b", "tail -f b.log", ""});

  auto counts_a = state.getProcessCounts("agent-a");
  auto counts_b = state.getProcessCounts("agent-b");
  EXPECT_EQ(counts_a.live, 0);
  EXPECT_EQ(counts_a.background, 1);
  EXPECT_EQ(counts_b.live, 0);
  EXPECT_EQ(counts_b.background, 1);
}

TEST(StreamStateManagerHistoryTest,
     ProcessStateLookupResolvesOriginAndFollowUpProcessTools) {
  StreamStateManager state;

  state.handleAgentToolCall(
      {"agent-1", "tool-origin", "Process", R"({"action":"Execute","command":"sleep 5"})", ""});
  state.handleAgentProcessSpawned(
      AgentProcessSpawned{"agent-1", "proc-1", "tool-origin", "sleep 5", ""});

  AgentTurn turn;
  turn.turnId = "turn-origin";
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "tool-origin",
      R"({"stdout":"","stderr":"","exit_code":-1,"duration_ms":1000,"finish_reason":"Timeout","process_id":"proc-1"})",
      true,
      "proc-1",
      ""}};
  turn.messages.push_back(tool_result);
  state.handleAgentTurnCompleted({"agent-1", turn, {}, ""});

  state.handleAgentToolCall(
      {"agent-1", "tool-wait", "Process",
       R"({"action":"Wait","process_id":"proc-1","pattern":"READY"})", ""});
  state.handleAgentToolCall(
      {"agent-1", "tool-status", "Process",
       R"({"action":"Status","process_id":"proc-1"})", ""});
  state.handleAgentToolCall(
      {"agent-1", "tool-input", "Process",
       R"({"action":"Input","process_id":"proc-1","input":"status\n"})", ""});

  const auto *origin = state.getProcessStateForToolCall("tool-origin");
  const auto *wait = state.getProcessStateForToolCall("tool-wait");
  const auto *status = state.getProcessStateForToolCall("tool-status");
  const auto *input = state.getProcessStateForToolCall("tool-input");

  ASSERT_NE(origin, nullptr);
  ASSERT_NE(wait, nullptr);
  ASSERT_NE(status, nullptr);
  ASSERT_NE(input, nullptr);
  EXPECT_EQ(origin->process_id, "proc-1");
  EXPECT_EQ(wait->process_id, "proc-1");
  EXPECT_EQ(status->process_id, "proc-1");
  EXPECT_EQ(input->process_id, "proc-1");
  EXPECT_EQ(wait->command, "sleep 5");
  EXPECT_TRUE(wait->is_background);
}

TEST(StreamStateManagerHistoryTest,
     DistinctAgentsRetainTranscriptAndShellStateAcrossFocusLikeSwitches) {
  StreamStateManager state;

  state.handleAgentToolCall(
      {"agent-a", "tool-a", "Process", R"({"action":"Spawn","command":"tail -f agent-a.log"})", ""});
  state.handleAgentProcessSpawned(
      AgentProcessSpawned{"agent-a", "proc-a", "tool-a", "tail -f agent-a.log", ""});
  state.handleAgentProcessOutput(
      AgentProcessOutput{"agent-a", "proc-a", "agent-a output\n", false, false, -1,
                         0.0, ""});

  AgentTurn turn_a;
  turn_a.turnId = "turn-a";
  Message assistant_a;
  assistant_a.role = Role::Assistant;
  assistant_a.content = {
      TextContent{"Agent A transcript"},
      ToolResultContent{"tool-a", R"({"process_id":"proc-a"})", true, "proc-a",
                        ""},
  };
  turn_a.messages.push_back(std::move(assistant_a));
  state.handleAgentTurnCompleted({"agent-a", turn_a, {}, ""});

  state.handleAgentToolCall(
      {"agent-b", "tool-b", "Process", R"({"action":"Spawn","command":"tail -f agent-b.log"})", ""});
  state.handleAgentProcessSpawned(
      AgentProcessSpawned{"agent-b", "proc-b", "tool-b", "tail -f agent-b.log", ""});
  state.handleAgentProcessOutput(
      AgentProcessOutput{"agent-b", "proc-b", "agent-b output\n", false, false, -1,
                         0.0, ""});

  AgentTurn turn_b;
  turn_b.turnId = "turn-b";
  Message assistant_b;
  assistant_b.role = Role::Assistant;
  assistant_b.content = {
      TextContent{"Agent B transcript"},
      ToolResultContent{"tool-b", R"({"process_id":"proc-b"})", true, "proc-b",
                        ""},
  };
  turn_b.messages.push_back(std::move(assistant_b));
  state.handleAgentTurnCompleted({"agent-b", turn_b, {}, ""});

  auto view_a = state.getToolView("tool-a");
  auto view_b = state.getToolView("tool-b");
  ASSERT_TRUE(static_cast<bool>(view_a));
  ASSERT_TRUE(static_cast<bool>(view_b));
  EXPECT_EQ(view_a->process_id, "proc-a");
  EXPECT_EQ(view_b->process_id, "proc-b");
  EXPECT_EQ(view_a->live_process_output, "agent-a output\n");
  EXPECT_EQ(view_b->live_process_output, "agent-b output\n");
  EXPECT_EQ(view_a->phase, ToolPhase::BackgroundRunning);
  EXPECT_EQ(view_b->phase, ToolPhase::BackgroundRunning);

  auto process_a = state.getProcessStateForToolCall("tool-a");
  auto process_b = state.getProcessStateForToolCall("tool-b");
  ASSERT_NE(process_a, nullptr);
  ASSERT_NE(process_b, nullptr);
  EXPECT_EQ(process_a->owner_agent_id, "agent-a");
  EXPECT_EQ(process_b->owner_agent_id, "agent-b");
  EXPECT_EQ(process_a->latest_output_tail, "agent-a output\n");
  EXPECT_EQ(process_b->latest_output_tail, "agent-b output\n");

  const auto counts_a = state.getProcessCounts("agent-a");
  const auto counts_b = state.getProcessCounts("agent-b");
  EXPECT_EQ(counts_a.live, 0);
  EXPECT_EQ(counts_a.background, 1);
  EXPECT_EQ(counts_b.live, 0);
  EXPECT_EQ(counts_b.background, 1);
}

TEST(StreamStateManagerHistoryTest,
     SubagentStateLookupResolvesSummonAndWaitForSameChildAgent) {
  StreamStateManager state;

  state.handleAgentToolCall({"parent", "summon-1", "Delegate",
                             R"({"name":"worker","title":"Worker","task":"Do task"})",
                             ""});
  state.handleAgentSpawned(
      {"child-1", "worker", "parent", "worker", "Worker", true, "", "", 0},
      "parent");
  state.handleAgentToolCall(
      {"parent", "wait-1", "Delegate", R"({"action":"Wait","agent_id":"child-1"})", ""});

  AgentTurn turn;
  turn.turnId = "turn-1";
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "wait-1",
      R"({"agentId":"child-1","status":"completed_no_summary","result":"","fallback_used":true,"attempted_categories":["executor","scout"],"category":"scout"})",
      true,
      "",
      ""}};
  turn.messages.push_back(tool_result);
  state.handleAgentTurnCompleted({"parent", turn, {}, ""});

  const auto *summon = state.getSubagentStateForToolCall("summon-1");
  const auto *wait = state.getSubagentStateForToolCall("wait-1");
  ASSERT_NE(summon, nullptr);
  ASSERT_NE(wait, nullptr);
  EXPECT_EQ(summon->child_agent_id, "child-1");
  EXPECT_EQ(wait->child_agent_id, "child-1");
  EXPECT_EQ(wait->wait_state, "completed_no_summary");
  EXPECT_TRUE(wait->fallback_used);
  EXPECT_EQ(wait->route_category, "scout");
}

TEST(StreamStateManagerHistoryTest,
     RebuildHistoryPopulatesNormalizedSubagentStateForSummonAndWait) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"summon-1", "Delegate",
                      R"({"title":"Worker","task":"Implement feature"})"},
      ToolCallContent{"wait-1", "Delegate", R"({"action":"Wait","agent_id":"child-1"})"},
  };
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {
      ToolResultContent{"summon-1",
                        R"({"agentId":"child-1","status":"spawned","category":"executor"})",
                        true, "", ""},
      ToolResultContent{
          "wait-1",
          R"({"agentId":"child-1","status":"completed","result":"done","artifacts_created":[{"reference":"@artifact:worker/report.md"}],"artifacts_updated":[]})",
          true, "", ""},
  };
  turn.messages.push_back(std::move(assistant));
  turn.messages.push_back(std::move(tool_result));
  history.turns.push_back(std::move(turn));

  StreamStateManager state;
  state.rebuildToolCallsFromHistory("parent", &history, "thread-1", false);

  const auto *summon = state.getSubagentStateForToolCall("summon-1");
  const auto *wait = state.getSubagentStateForToolCall("wait-1");
  ASSERT_NE(summon, nullptr);
  ASSERT_NE(wait, nullptr);
  EXPECT_EQ(summon->child_agent_id, "child-1");
  EXPECT_EQ(summon->task, "Implement feature");
  EXPECT_EQ(wait->wait_state, "completed");
  ASSERT_EQ(wait->artifacts_created.size(), 1u);
  EXPECT_NE(wait->artifacts_created[0].find("@artifact:worker/report.md"),
            std::string::npos);
}

TEST(StreamStateManagerHistoryTest,
     RebuildPreservesTypedCompletedNoSummaryAndArtifacts) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-typed-nosummary";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"wait-typed", "Delegate", R"({"action":"Wait","agent_id":"child-typed"})"},
  };
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {
      ToolResultContent{
          "wait-typed",
          R"({"agentId":"child-typed","status":"completed_no_summary","result":"","fallback_used":true,"attempted_categories":["executor","scout"],"category":"scout","artifacts_created":[{"reference":"@artifact:worker/out.md"}],"artifacts_updated":[{"reference":"@artifact:worker/state.json"}]})",
          true, "", ""},
  };
  turn.messages.push_back(std::move(assistant));
  turn.messages.push_back(std::move(tool_result));
  history.turns.push_back(std::move(turn));

  StreamStateManager state;
  state.rebuildToolCallsFromHistory("parent", &history, "thread-1", false);

  const auto *wait = state.getSubagentStateForToolCall("wait-typed");
  ASSERT_NE(wait, nullptr);
  EXPECT_EQ(wait->wait_state, "completed_no_summary");
  EXPECT_TRUE(wait->fallback_used);
  EXPECT_EQ(wait->route_category, "scout");
  ASSERT_EQ(wait->attempted_categories.size(), 2u);
  ASSERT_EQ(wait->artifacts_created.size(), 1u);
  ASSERT_EQ(wait->artifacts_updated.size(), 1u);
}

TEST(StreamStateManagerHistoryTest, RebuildPreservesTypedCancelledOutcome) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-typed-cancelled";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"wait-cancelled", "Delegate",
                      R"({"agent_id":"child-cancelled"})"},
  };
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "wait-cancelled",
      R"({"agentId":"child-cancelled","status":"cancelled","result":"Cancelled by parent."})",
      true, "", ""}};
  turn.messages.push_back(std::move(assistant));
  turn.messages.push_back(std::move(tool_result));
  history.turns.push_back(std::move(turn));

  StreamStateManager state;
  state.rebuildToolCallsFromHistory("parent", &history, "thread-1", false);

  const auto *wait = state.getSubagentStateForToolCall("wait-cancelled");
  ASSERT_NE(wait, nullptr);
  EXPECT_EQ(wait->wait_state, "cancelled");
  EXPECT_EQ(wait->outcome, firmius::tui::SubagentOutcomeKind::Cancelled);
}

TEST(StreamStateManagerHistoryTest, RebuildPreservesTypedSpawnedRouteState) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-typed-spawned";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"summon-spawned", "Delegate",
                      R"({"title":"Worker","task":"Investigate"})"},
  };
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {ToolResultContent{
      "summon-spawned",
      R"({"agentId":"child-spawned","status":"spawned","fallback_used":false,"category":"executor","attempted_categories":["executor"]})",
      true, "", ""}};
  turn.messages.push_back(std::move(assistant));
  turn.messages.push_back(std::move(tool_result));
  history.turns.push_back(std::move(turn));

  StreamStateManager state;
  state.rebuildToolCallsFromHistory("parent", &history, "thread-1", false);

  const auto *summon = state.getSubagentStateForToolCall("summon-spawned");
  ASSERT_NE(summon, nullptr);
  EXPECT_EQ(summon->wait_state, "spawned");
  EXPECT_TRUE(summon->running);
  EXPECT_EQ(summon->route_category, "executor");
  ASSERT_EQ(summon->attempted_categories.size(), 1u);
}

TEST(StreamStateManagerHistoryTest,
     RebuildOutOfOrderWaitThenSummonMergesToSingleParentState) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-out-of-order";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"wait-1", "Delegate", R"({"action":"Wait","agent_id":"child-1"})"},
      ToolCallContent{"summon-1", "Delegate",
                      R"({"title":"Worker","task":"Inspect requirements"})"},
  };
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {
      ToolResultContent{
          "wait-1",
          R"({"agentId":"child-1","status":"completed","result":"Worker summary from reload","artifacts_created":[{"reference":"@artifact:worker/report.md"}]})",
          true, "", ""},
      ToolResultContent{
          "summon-1",
          R"({"agentId":"child-1","status":"spawned","category":"executor"})",
          true, "", ""},
  };
  turn.messages.push_back(std::move(assistant));
  turn.messages.push_back(std::move(tool_result));
  history.turns.push_back(std::move(turn));

  StreamStateManager state;
  state.rebuildToolCallsFromHistory("parent", &history, "thread-1", false);

  const auto *summon = state.getSubagentStateForToolCall("summon-1");
  const auto *wait = state.getSubagentStateForToolCall("wait-1");
  ASSERT_NE(summon, nullptr);
  ASSERT_NE(wait, nullptr);
  EXPECT_EQ(summon, wait);
  EXPECT_EQ(summon->parent_tool_call_id, "summon-1");
  EXPECT_EQ(summon->wait_state, "completed");
  EXPECT_EQ(summon->final_summary, "Worker summary from reload");
  ASSERT_EQ(summon->artifacts_created.size(), 1u);
  EXPECT_NE(summon->artifacts_created.front().find("@artifact:worker/report.md"),
            std::string::npos);
}

TEST(StreamStateManagerHistoryTest,
     RebuildPopulateSubagentLogKeepsWaitSummaryOnSummonState) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-populate";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"summon-2", "Delegate",
                      R"({"title":"Worker","task":"Inspect requirements"})"},
      ToolCallContent{"wait-2", "Delegate", R"({"action":"Wait","agent_id":"child-2"})"},
  };
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {
      ToolResultContent{
          "summon-2",
          R"({"agentId":"child-2","status":"spawned","category":"executor"})",
          true, "", ""},
      ToolResultContent{
          "wait-2",
          R"({"agentId":"child-2","status":"completed","result":"Meaningful persisted summary after reload"})",
          true, "", ""},
  };
  turn.messages.push_back(std::move(assistant));
  turn.messages.push_back(std::move(tool_result));
  history.turns.push_back(std::move(turn));

  StreamStateManager state;
  state.rebuildToolCallsFromHistory("parent", &history, "thread-missing", true);

  const auto *summon = state.getSubagentStateForToolCall("summon-2");
  ASSERT_NE(summon, nullptr);
  EXPECT_EQ(summon->wait_state, "completed");
  EXPECT_EQ(summon->final_summary,
            "Meaningful persisted summary after reload");
}

TEST(StreamStateManagerHistoryTest,
     RebuildPopulateSubagentLogCarriesChildToolHistoryAcrossWholeThreadReload) {
  AgentTurn child_turn;
  child_turn.turnId = "child-turn-1";
  Message child_assistant;
  child_assistant.role = Role::Assistant;
  child_assistant.content = {
      ToolCallContent{"child-list", "Files", R"({"action":"List","path":"."})"},
      ToolCallContent{"child-read", "Artifacts",
                      R"({"action":"Read","reference":"@artifact:dir-researcher/WORKER_REPORT.md"})"},
  };
  Message child_result;
  child_result.role = Role::ToolResult;
  child_result.content = {
      ToolResultContent{"child-list", R"([{"name":"REQUIREMENTS.md"}])", true, "", ""},
      ToolResultContent{
          "child-read",
          R"({"reference":"@artifact:dir-researcher/WORKER_REPORT.md","artifact":{"filename":"WORKER_REPORT.md","owner_friendly_name":"dir-researcher"}})",
          true, "", ""},
  };
  child_turn.messages.push_back(std::move(child_assistant));
  child_turn.messages.push_back(std::move(child_result));
  AgentHistory child_history;
  child_history.turns.push_back(std::move(child_turn));

  AgentHistory parent_history;
  AgentTurn parent_turn;
  parent_turn.turnId = "parent-turn-1";
  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"summon-1", "Delegate",
                      R"({"title":"Worker","task":"Inspect requirements"})"},
      ToolCallContent{"wait-1", "Delegate", R"({"action":"Wait","agent_id":"child-1"})"},
  };
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {
      ToolResultContent{
          "summon-1",
          R"({"agentId":"child-1","status":"spawned","category":"executor"})",
          true, "", ""},
      ToolResultContent{
          "wait-1",
          R"({"agentId":"child-1","status":"completed","result":"Worker summary from reload"})",
          true, "", ""},
  };
  parent_turn.messages.push_back(std::move(assistant));
  parent_turn.messages.push_back(std::move(tool_result));
  parent_history.turns.push_back(std::move(parent_turn));

  StreamStateManager state;
  state.rebuildToolCallsFromHistory("parent", &parent_history, "thread-1", false);
  state.rebuildToolCallsFromHistory("child-1", &child_history, "thread-1", false);
  state.rebuildToolCallsFromHistory("parent", &parent_history, "thread-1", true);

  auto view = state.getToolView("summon-1");
  ASSERT_TRUE(static_cast<bool>(view));
  ASSERT_GE(view->subagent_tool_log.size(), 3u);
  EXPECT_TRUE(std::any_of(
      view->subagent_tool_log.begin(), view->subagent_tool_log.end(),
      [](const firmius::shared::SubagentToolLogEntry &entry) {
        return entry.summary == "List .";
      }));
  EXPECT_TRUE(std::any_of(
      view->subagent_tool_log.begin(), view->subagent_tool_log.end(),
      [](const firmius::shared::SubagentToolLogEntry &entry) {
        return entry.summary ==
               "Read @artifact:dir-researcher/WORKER_REPORT.md";
      }));
  EXPECT_EQ(view->subagent_tool_log.back().summary, "Done");
}

} // namespace
