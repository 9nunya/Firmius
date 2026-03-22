#include "components/ChatWindow.hpp"
#include "components/ToolBlock.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace {

using firmius::shared::AgentHistory;
using firmius::shared::AgentTurn;
using firmius::shared::Message;
using firmius::shared::Role;
using firmius::shared::TextContent;
using firmius::shared::ToolCallContent;
using firmius::shared::ToolResultContent;
using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;
using firmius::tui::TimelineEntry;

std::string renderToString(ftxui::Element element, int width = 80,
                           int height = 6) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  Render(screen, element);
  return screen.ToString();
}

std::string renderComponentToString(const ftxui::Component &component,
                                    int width = 120, int height = 16) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  Render(screen, component->Render());
  // ScrollableBox-based components need a second pass after reflect() sets the
  // viewport box from the first layout pass.
  Render(screen, component->Render());
  return screen.ToString();
}

TEST(ChatWindowHelpersTest, HidesPreparingRowsWithoutUsableName) {
  ToolCallView view;
  view.phase = ToolPhase::Preparing;
  view.name = "   ";
  view.args = R"({"path":"a.txt"})";

  EXPECT_FALSE(firmius::tui::ShouldRenderToolCallView(view));

  view.name = "file_edit";
  EXPECT_TRUE(firmius::tui::ShouldRenderToolCallView(view));

  view.phase = ToolPhase::Called;
  view.name = "";
  EXPECT_FALSE(firmius::tui::ShouldRenderToolCallView(view));
}

TEST(ChatWindowHelpersTest, HidesCalledRowsWithoutUsableNameEvenWhenArgsExist) {
  ToolCallView view;
  view.phase = ToolPhase::Called;
  view.name = "  ";
  view.args = R"({"path":"a.txt"})";

  EXPECT_FALSE(firmius::tui::ShouldRenderToolCallView(view));

  view.name = "file_read";
  EXPECT_TRUE(firmius::tui::ShouldRenderToolCallView(view));
}

TEST(ChatWindowHelpersTest, CollectsHistoryToolCallIdsForLiveDedupe) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      TextContent{"working"},
      ToolCallContent{"call-summon", "summon_subagent", R"({"task":"a"})"},
      ToolCallContent{"call-wait", "subagent_wait", R"({"agent_id":"sub-1"})"},
      ToolCallContent{"call-summon", "summon_subagent", R"({"task":"a"})"},
  };
  turn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(turn));

  const auto ids = firmius::tui::CollectToolCallIdsFromHistory(&history);

  EXPECT_EQ(ids.size(), 2u);
  EXPECT_TRUE(ids.count("call-summon") > 0);
  EXPECT_TRUE(ids.count("call-wait") > 0);
}

TEST(ChatWindowHelpersTest,
     CollectsHistoryToolCallIdsFromToolResultsForLiveDedupe) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  Message tool_result_msg;
  tool_result_msg.role = Role::ToolResult;
  tool_result_msg.content = {
      ToolResultContent{"call-summon", R"({"agentId":"sub-1"})", true, "", ""},
      ToolResultContent{"call-wait", R"({"done":true})", true, "", ""},
      ToolResultContent{"call-summon", R"({"agentId":"sub-1"})", true, "", ""},
  };

  turn.messages.push_back(std::move(tool_result_msg));
  history.turns.push_back(std::move(turn));

  const auto ids = firmius::tui::CollectToolCallIdsFromHistory(&history);

  EXPECT_EQ(ids.size(), 2u);
  EXPECT_TRUE(ids.count("call-summon") > 0);
  EXPECT_TRUE(ids.count("call-wait") > 0);
}

TEST(ChatWindowHelpersTest, KeepsPersistedCompactionTurnsRenderable) {
  Message compaction;
  compaction.role = Role::System;
  compaction.content = {TextContent{"Compaction started."}};

  EXPECT_FALSE(firmius::tui::ShouldHideMessageInTranscript(
      compaction, false, "compaction-start-1"));

  compaction.content = {TextContent{"Persisted compaction snapshot."}};
  EXPECT_FALSE(firmius::tui::ShouldHideMessageInTranscript(
      compaction, false, "compaction-summary-1"));

  compaction.content = {TextContent{"Compaction completed."}};
  EXPECT_FALSE(firmius::tui::ShouldHideMessageInTranscript(
      compaction, false, "compaction-end-1"));
}

TEST(ChatWindowHelpersTest, IndentsAgentRenderedContentWithoutMarker) {
  auto output = renderToString(
      firmius::tui::IndentAgentRow(ftxui::text("agent row")));

  EXPECT_NE(output.find("  agent row"), std::string::npos);
  EXPECT_EQ(output.find("* "), std::string::npos);
  EXPECT_EQ(output.find("[parent]"), std::string::npos);
}

TEST(ChatWindowHelpersTest, FocusedSubagentToolCallIgnoresParentSummons) {
  TimelineEntry parent_entry;
  parent_entry.kind = TimelineEntry::Kind::ToolCall;
  parent_entry.id = "tool-parent";
  parent_entry.agentId = "parent-agent";

  ToolCallView summon_view;
  summon_view.name = "summon_subagent";
  summon_view.subagent_id = "child-agent";

  EXPECT_FALSE(firmius::tui::ShouldRenderFocusedSubagentToolCall(
      parent_entry, summon_view, "child-agent"));

  TimelineEntry child_entry;
  child_entry.kind = TimelineEntry::Kind::ToolCall;
  child_entry.id = "tool-child";
  child_entry.agentId = "child-agent";

  ToolCallView child_view;
  child_view.name = "file_read";

  EXPECT_TRUE(firmius::tui::ShouldRenderFocusedSubagentToolCall(
      child_entry, child_view, "child-agent"));
}

TEST(ChatWindowHelpersTest,
     HistoryProcessExecuteUsesNormalizedStateFactsViaGetter) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"exec-1", "process_execute", R"({"command":"sleep 5"})"},
  };
  turn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(turn));

  firmius::tui::NormalizedProcessState process;
  process.process_id = "proc-1";
  process.command = "sleep 5";
  process.running = true;
  process.is_background = true;

  auto chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; },
      nullptr,
      [](const std::string &) -> std::shared_ptr<ToolCallView> { return nullptr; },
      [&process](const std::string &tool_call_id)
          -> const firmius::tui::NormalizedProcessState * {
        if (tool_call_id == "exec-1") {
          return &process;
        }
        return nullptr;
      });

  const std::string output = renderComponentToString(chat);
  EXPECT_NE(output.find("proc-1"), std::string::npos);
  EXPECT_NE(output.find("sleep 5"), std::string::npos);
}

TEST(ChatWindowHelpersTest,
     HistoryProcessWaitUsesNormalizedStateFactsViaGetter) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"wait-1", "process_wait",
                      R"({"process_id":"proc-1","pattern":"READY"})"},
  };
  turn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(turn));

  firmius::tui::NormalizedProcessState process;
  process.process_id = "proc-1";
  process.command = "sleep 5";
  process.waiting = true;
  process.waiting_pattern = "READY";

  auto chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; },
      nullptr,
      [](const std::string &) -> std::shared_ptr<ToolCallView> { return nullptr; },
      [&process](const std::string &tool_call_id)
          -> const firmius::tui::NormalizedProcessState * {
        if (tool_call_id == "wait-1") {
          return &process;
        }
        return nullptr;
      });

  const std::string output = renderComponentToString(chat);
  EXPECT_NE(output.find("proc-1"), std::string::npos);
  EXPECT_NE(output.find("sleep 5"), std::string::npos);
  EXPECT_NE(output.find("READY"), std::string::npos);
}

TEST(ChatWindowHelpersTest,
     LiveAndHistoryProcessFactsStayEquivalentOnKeyFields) {
  auto view = std::make_shared<ToolCallView>();
  view->toolCallId = "status-1";
  view->name = "process_status";
  view->args = R"({"process_id":"proc-1"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->result = R"({"isRunning":false,"exitCode":0,"duration_ms":100})";

  firmius::tui::NormalizedProcessState process;
  process.process_id = "proc-1";
  process.command = "sleep 5";
  process.finished = true;
  process.exit_code_known = true;
  process.exit_code = 0;
  process.duration_ms = 100.0;

  auto live_block = firmius::tui::ToolBlock(
      view, nullptr, nullptr,
      [&process](const std::string &) -> const firmius::tui::NormalizedProcessState * {
        return &process;
      });
  const std::string live_output = renderComponentToString(live_block);

  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";
  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"status-1", "process_status", R"({"process_id":"proc-1"})"},
  };
  turn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(turn));

  auto history_chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; },
      nullptr,
      [view](const std::string &) { return view; },
      [&process](const std::string &) -> const firmius::tui::NormalizedProcessState * {
        return &process;
      });
  const std::string history_output = renderComponentToString(history_chat);

  EXPECT_NE(live_output.find("proc-1"), std::string::npos);
  EXPECT_NE(history_output.find("proc-1"), std::string::npos);
  EXPECT_NE(live_output.find("sleep 5"), std::string::npos);
  EXPECT_NE(history_output.find("sleep 5"), std::string::npos);
}

TEST(ChatWindowHelpersTest,
     HistorySummonSubagentUsesNormalizedStateFactsViaGetter) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";
  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"summon-1", "summon_subagent",
                      R"({"title":"Worker","task":"Implement login"})"},
  };
  turn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(turn));

  firmius::tui::NormalizedSubagentState subagent;
  subagent.parent_tool_call_id = "summon-1";
  subagent.child_agent_id = "child-1";
  subagent.child_title = "Worker";
  subagent.wait_state = "completed";
  subagent.final_summary = "Done";
  subagent.artifacts_created = {"@artifact:worker/report.md"};

  auto chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; }, nullptr,
      [](const std::string &) -> std::shared_ptr<ToolCallView> { return nullptr; },
      nullptr,
      [&subagent](const std::string &tool_call_id)
          -> const firmius::tui::NormalizedSubagentState * {
        return tool_call_id == "summon-1" ? &subagent : nullptr;
      });

  const std::string output = renderComponentToString(chat);
  EXPECT_NE(output.find("Worker"), std::string::npos);
  EXPECT_NE(output.find("child-1"), std::string::npos);
  EXPECT_NE(output.find("+1 artifact"), std::string::npos);
}

TEST(ChatWindowHelpersTest,
     LiveAndHistorySubagentFactsStayEquivalentOnKeyFields) {
  auto view = std::make_shared<ToolCallView>();
  view->toolCallId = "wait-1";
  view->name = "subagent_wait";
  view->args = R"({"agent_id":"child-1"})";
  view->phase = ToolPhase::Finished;
  view->success = true;

  firmius::tui::NormalizedSubagentState subagent;
  subagent.parent_tool_call_id = "summon-1";
  subagent.child_agent_id = "child-1";
  subagent.child_title = "Worker";
  subagent.wait_state = "completed";
  subagent.final_summary = "Done";

  auto live_block = firmius::tui::ToolBlock(
      view, nullptr, nullptr, nullptr,
      [&subagent](const std::string &) -> const firmius::tui::NormalizedSubagentState * {
        return &subagent;
      });
  const std::string live_output = renderComponentToString(live_block);

  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";
  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"wait-1", "subagent_wait", R"({"agent_id":"child-1"})"},
  };
  turn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(turn));

  auto history_chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; }, nullptr,
      [view](const std::string &) { return view; }, nullptr,
      [&subagent](const std::string &) -> const firmius::tui::NormalizedSubagentState * {
        return &subagent;
      });
  const std::string history_output = renderComponentToString(history_chat);

  EXPECT_NE(live_output.find("waiting"), std::string::npos);
  EXPECT_NE(history_output.find("waiting"), std::string::npos);
  EXPECT_NE(live_output.find("Worker"), std::string::npos);
  EXPECT_NE(history_output.find("Worker"), std::string::npos);
}

TEST(ChatWindowHelpersTest,
     HistoryFileAndListToolsRenderThroughCentralPresentationPath) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"read-1", "file_read",
                      R"({"path":"src/main.cpp","start_line":1,"end_line":3})"},
      ToolCallContent{"ls-1", "list_directory", R"({"path":"src"})"},
  };
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {
      ToolResultContent{"read-1",
                        R"({"content":"a\nb\nc","line_start":1,"line_end":3,"lines_read":3})",
                        true, "", ""},
      ToolResultContent{"ls-1",
                        R"([{"name":"core","is_directory":true},{"name":"main.cpp","is_directory":false}])",
                        true, "", ""},
  };
  turn.messages.push_back(std::move(assistant));
  turn.messages.push_back(std::move(tool_result));
  history.turns.push_back(std::move(turn));

  auto chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; }, nullptr,
      [](const std::string &) -> std::shared_ptr<ToolCallView> { return nullptr; });

  const std::string output = renderComponentToString(chat, 140, 24);
  EXPECT_NE(output.find("Read"), std::string::npos);
  EXPECT_NE(output.find("src/main.cpp:1-3"), std::string::npos);
  EXPECT_NE(output.find("Listed"), std::string::npos);
  EXPECT_NE(output.find("src"), std::string::npos);
}

} // namespace
