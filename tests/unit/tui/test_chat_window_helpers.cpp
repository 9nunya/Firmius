#include "components/ChatWindow.hpp"
#include "components/ToolBlock.hpp"
#include "persistence/ThreadManager.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"

#include <ftxui/component/event.hpp>
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
using firmius::shared::NoticeContent;
using firmius::shared::NoticeSeverity;

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

  view.name = "Read";
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
      ToolCallContent{"call-summon", "Delegate", R"({"action":"Spawn","task":"a"})"},
      ToolCallContent{"call-wait", "Delegate", R"({"action":"Wait","agent_id":"sub-1"})"},
      ToolCallContent{"call-summon", "Delegate", R"({"action":"Spawn","task":"a"})"},
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

TEST(ChatWindowHelpersTest, HiddenChatErrorNotificationOnlyFiresForFocusedAgent) {
  EXPECT_TRUE(firmius::tui::detail::shouldNotifyHiddenChatError(
      "agent-1", "agent-1", true));
  EXPECT_FALSE(firmius::tui::detail::shouldNotifyHiddenChatError(
      "agent-1", "agent-2", true));
  EXPECT_FALSE(firmius::tui::detail::shouldNotifyHiddenChatError(
      "agent-1", "agent-1", false));
  EXPECT_FALSE(firmius::tui::detail::shouldNotifyHiddenChatError(
      "", "agent-1", true));
}

TEST(ChatWindowHelpersTest,
     CollectsHistoryToolCallIdsFromAssistantEmbeddedToolResultsForLiveDedupe) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"call-stop", "Delegate",
                      R"({"action":"Stop","agent_id":"sub-1"})"},
      ToolResultContent{"call-stop", R"({"status":"terminated"})", true, "",
                        ""},
      ToolCallContent{"call-spawn", "Delegate",
                      R"({"action":"Spawn","task":"route"})"},
      ToolResultContent{"call-spawn", R"({"agentId":"sub-2"})", true, "", ""},
  };
  turn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(turn));

  const auto ids = firmius::tui::CollectToolCallIdsFromHistory(&history);
  EXPECT_TRUE(ids.count("call-stop") > 0);
  EXPECT_TRUE(ids.count("call-spawn") > 0);
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

TEST(ChatWindowHelpersTest,
     FinishedRowsWithoutRenderableIdentityStayHidden) {
  ToolCallView view;
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.name = "";
  view.result = R"({"ok":true})";

  EXPECT_FALSE(firmius::tui::ShouldRenderToolCallView(view));
}

TEST(ChatWindowHelpersTest,
     ExpandCompactionTranscriptKeepsPreCompactionTailBeforeMarker) {
  using firmius::core::CompactionSnapshot;

  std::vector<AgentTurn> snapshot_turns;

  auto makeTurn = [](std::string id, Role role, std::string text) {
    AgentTurn turn;
    turn.turnId = std::move(id);
    Message msg;
    msg.role = role;
    msg.content = {TextContent{std::move(text)}};
    turn.messages.push_back(std::move(msg));
    return turn;
  };

  snapshot_turns.push_back(
      makeTurn("bootstrap-system", Role::System, "bootstrap"));
  snapshot_turns.push_back(makeTurn("user-task-1", Role::User, "first user"));
  snapshot_turns.push_back(
      makeTurn("assistant-2", Role::Assistant, "first answer"));
  snapshot_turns.push_back(
      makeTurn("assistant-22", Role::Assistant, "final summary before compact"));

  std::vector<AgentTurn> compacted_turns;
  compacted_turns.push_back(snapshot_turns.front());
  compacted_turns.push_back(
      makeTurn("compaction-start-1", Role::System, "Compaction started."));
  compacted_turns.push_back(
      makeTurn("compaction-summary-1", Role::System, "Compaction summary."));
  compacted_turns.push_back(
      makeTurn("compaction-end-1", Role::System, "Compaction complete."));
  compacted_turns.push_back(snapshot_turns.back());
  compacted_turns.push_back(
      makeTurn("user-task-7", Role::User, "Can you see compaction?"));

  CompactionSnapshot snapshot;
  snapshot.compactionId = "1";
  snapshot.turns = snapshot_turns;

  std::unordered_map<std::string, CompactionSnapshot> snapshots;
  snapshots.emplace("1", snapshot);

  const auto expanded = firmius::tui::expandCompactionTranscriptForDisplay(
      compacted_turns, snapshots);

  ASSERT_EQ(expanded.size(), 8u);
  EXPECT_EQ(expanded[0].turnId, "bootstrap-system");
  EXPECT_EQ(expanded[1].turnId, "user-task-1");
  EXPECT_EQ(expanded[2].turnId, "assistant-2");
  EXPECT_EQ(expanded[3].turnId, "assistant-22");
  EXPECT_EQ(expanded[4].turnId, "compaction-start-1");
  EXPECT_EQ(expanded[5].turnId, "compaction-summary-1");
  EXPECT_EQ(expanded[6].turnId, "compaction-end-1");
  EXPECT_EQ(expanded[7].turnId, "user-task-7");

}

TEST(ChatWindowHelpersTest,
     TranscriptGrowthAtBottomDoesNotLeaveBlankGapUntilEndKey) {
  AgentHistory history;

  auto make_assistant_turn = [](std::string id, std::string text) {
    AgentTurn turn;
    turn.turnId = std::move(id);
    Message msg;
    msg.role = Role::Assistant;
    msg.content = {TextContent{std::move(text)}};
    turn.messages.push_back(std::move(msg));
    return turn;
  };

  for (int i = 0; i < 18; ++i) {
    history.turns.push_back(make_assistant_turn(
        "turn-" + std::to_string(i),
        "assistant row " + std::to_string(i) +
            " with enough trailing words to wrap across multiple screen lines"));
  }

  auto chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; });

  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(36),
                                      ftxui::Dimension::Fixed(8));
  Render(screen, chat->Render());
  Render(screen, chat->Render());

  history.turns.push_back(make_assistant_turn(
      "turn-new",
      "fresh bottom row with enough trailing words to wrap across multiple screen lines and prove the viewport stayed pinned"));

  EXPECT_TRUE(chat->OnEvent(ftxui::Event::Special("TranscriptChanged")));

  Render(screen, chat->Render());
  const auto first_pass = screen.ToString();
  Render(screen, chat->Render());
  const auto second_pass = screen.ToString();

  EXPECT_NE(first_pass.find("fresh"), std::string::npos) << first_pass;
  EXPECT_NE(second_pass.find("fresh"), std::string::npos) << second_pass;
  EXPECT_NE(first_pass.find("pinned"), std::string::npos) << first_pass;
  EXPECT_NE(second_pass.find("pinned"), std::string::npos) << second_pass;

  const std::string blank_band = "                                    \n"
                                 "                                    \n"
                                 "                                    \n";
  EXPECT_EQ(first_pass.find(blank_band), std::string::npos) << first_pass;
  EXPECT_EQ(second_pass.find(blank_band), std::string::npos) << second_pass;
}


TEST(ChatWindowHelpersTest,
     TranscriptGrowthWhileScrolledUpKeepsManualOffsetStable) {
  AgentHistory history;

  auto make_assistant_turn = [](std::string id, std::string text) {
    AgentTurn turn;
    turn.turnId = std::move(id);
    Message msg;
    msg.role = Role::Assistant;
    msg.content = {TextContent{std::move(text)}};
    turn.messages.push_back(std::move(msg));
    return turn;
  };

  for (int i = 0; i < 24; ++i) {
    history.turns.push_back(make_assistant_turn(
        "turn-" + std::to_string(i),
        "assistant row " + std::to_string(i) +
            " with enough trailing words to wrap across multiple screen lines"));
  }

  auto chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; });

  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(36),
                                      ftxui::Dimension::Fixed(8));
  Render(screen, chat->Render());
  Render(screen, chat->Render());

  EXPECT_TRUE(chat->OnEvent(ftxui::Event::PageUp));
  EXPECT_TRUE(chat->OnEvent(ftxui::Event::PageUp));
  const auto before_growth = renderComponentToString(chat, 36, 8);

  history.turns.push_back(make_assistant_turn(
      "turn-new",
      "fresh bottom row with enough trailing words to wrap across multiple screen lines and prove manual scroll stays put"));

  EXPECT_TRUE(chat->OnEvent(ftxui::Event::Special("TranscriptChanged")));

  const auto after_growth = renderComponentToString(chat, 36, 8);

  EXPECT_NE(before_growth.find("assistant row 15"), std::string::npos)
      << before_growth;
  EXPECT_EQ(after_growth.find("fresh"), std::string::npos) << after_growth;
  EXPECT_EQ(before_growth, after_growth);
  EXPECT_EQ(after_growth.find("assistant row 21"), std::string::npos)
      << after_growth;
}
TEST(TuiStateChatSignatureTest, ChangesWhenQueuedMessagesChange) {
  using firmius::tui::StreamStateManager;

  StreamStateManager mgr;

  // NOTE: no persisted tool calls for this test.
  const std::unordered_set<std::string> persisted;
  const std::string focused_agent_id = "agent-1";
  const std::string thread_id = "thread-1";

  const auto base = firmius::tui::BuildFocusedChatLiveMeasurementSignature(
      mgr, focused_agent_id, thread_id, persisted);

  // Inject one queued message by simulating the queue event.
  firmius::shared::MessageQueued q;
  q.messageId = "msg-1";
  q.text = "hello queued";
  q.threadId = thread_id;
  q.agentId = focused_agent_id;
  mgr.handleMessageQueued(q);

  const auto with_queued = firmius::tui::BuildFocusedChatLiveMeasurementSignature(
      mgr, focused_agent_id, thread_id, persisted);

  EXPECT_NE(base, with_queued);
}


TEST(ChatWindowHelpersTest,
     ExpandCompactionTranscriptSkipsDuplicatedPreservedTailButKeepsLaterTurns) {
  using firmius::core::CompactionSnapshot;

  auto makeTurn = [](std::string id, Role role, std::string text) {
    AgentTurn turn;
    turn.turnId = std::move(id);
    Message msg;
    msg.role = role;
    msg.content = {TextContent{std::move(text)}};
    turn.messages.push_back(std::move(msg));
    return turn;
  };

  std::vector<AgentTurn> snapshot_turns{
      makeTurn("bootstrap-system", Role::System, "bootstrap"),
      makeTurn("user-task-1", Role::User, "first user"),
      makeTurn("assistant-2", Role::Assistant, "answer"),
      makeTurn("assistant-22", Role::Assistant, "summary before compact"),
  };

  std::vector<AgentTurn> compacted_turns{
      snapshot_turns.front(),
      makeTurn("compaction-start-1", Role::System, "start"),
      makeTurn("compaction-summary-1", Role::System, "summary"),
      makeTurn("compaction-end-1", Role::System, "end"),
      snapshot_turns.back(),
      makeTurn("user-task-7", Role::User, "post compact question"),
      makeTurn("assistant-8", Role::Assistant, "post compact answer"),
  };

  CompactionSnapshot snapshot;
  snapshot.compactionId = "1";
  snapshot.turns = snapshot_turns;

  std::unordered_map<std::string, CompactionSnapshot> snapshots;
  snapshots.emplace("1", snapshot);

  const auto expanded = firmius::tui::expandCompactionTranscriptForDisplay(
      compacted_turns, snapshots);

  ASSERT_EQ(expanded.size(), 9u);
  EXPECT_EQ(expanded[3].turnId, "assistant-22");
  EXPECT_EQ(expanded[4].turnId, "compaction-start-1");
  EXPECT_EQ(expanded[5].turnId, "compaction-summary-1");
  EXPECT_EQ(expanded[6].turnId, "compaction-end-1");
  EXPECT_EQ(expanded[7].turnId, "user-task-7");
  EXPECT_EQ(expanded[8].turnId, "assistant-8");
}

TEST(ChatWindowHelpersTest,
     ExpandCompactionTranscriptPreservesToolTailAndSkipsDuplicatedReplay) {
  using firmius::core::CompactionSnapshot;

  auto makeTextTurn = [](std::string id, Role role, std::string text) {
    AgentTurn turn;
    turn.turnId = std::move(id);
    Message msg;
    msg.role = role;
    msg.content = {TextContent{std::move(text)}};
    turn.messages.push_back(std::move(msg));
    return turn;
  };

  auto makeToolTurn = [](std::string id, std::string call_id) {
    AgentTurn turn;
    turn.turnId = std::move(id);
    Message msg;
    msg.role = Role::ToolResult;
    msg.content = {ToolResultContent{std::move(call_id), "done", true, "", ""}};
    turn.messages.push_back(std::move(msg));
    return turn;
  };

  std::vector<AgentTurn> snapshot_turns{
      makeTextTurn("bootstrap-system", Role::System, "bootstrap"),
      makeTextTurn("user-task-1", Role::User, "first user"),
      makeTextTurn("assistant-2", Role::Assistant, "answer"),
      makeToolTurn("tools-21", "call-21"),
      makeTextTurn("assistant-22", Role::Assistant, "summary before compact"),
      makeTextTurn("user-task-23", Role::User, "question before compact"),
  };

  std::vector<AgentTurn> compacted_turns{
      snapshot_turns.front(),
      makeTextTurn("compaction-start-1", Role::System, "start"),
      makeTextTurn("compaction-summary-1", Role::System, "summary"),
      makeTextTurn("compaction-end-1", Role::System, "end"),
      makeToolTurn("tools-21", "call-21"),
      makeTextTurn("assistant-22", Role::Assistant, "summary before compact"),
      makeTextTurn("user-task-23", Role::User, "question before compact"),
      makeTextTurn("user-task-7", Role::User, "post compact question"),
      makeTextTurn("assistant-8", Role::Assistant, "post compact answer"),
  };

  CompactionSnapshot snapshot;
  snapshot.compactionId = "1";
  snapshot.turns = snapshot_turns;

  std::unordered_map<std::string, CompactionSnapshot> snapshots;
  snapshots.emplace("1", snapshot);

  const auto expanded = firmius::tui::expandCompactionTranscriptForDisplay(
      compacted_turns, snapshots);

  ASSERT_EQ(expanded.size(), 11u);
  EXPECT_EQ(expanded[0].turnId, "bootstrap-system");
  EXPECT_EQ(expanded[1].turnId, "user-task-1");
  EXPECT_EQ(expanded[2].turnId, "assistant-2");
  EXPECT_EQ(expanded[3].turnId, "tools-21");
  EXPECT_EQ(expanded[4].turnId, "assistant-22");
  EXPECT_EQ(expanded[5].turnId, "user-task-23");
  EXPECT_EQ(expanded[6].turnId, "compaction-start-1");
  EXPECT_EQ(expanded[7].turnId, "compaction-summary-1");
  EXPECT_EQ(expanded[8].turnId, "compaction-end-1");
  EXPECT_EQ(expanded[9].turnId, "user-task-7");
  EXPECT_EQ(expanded[10].turnId, "assistant-8");
}

TEST(ChatWindowHelpersTest, KeepsSystemNoteTurnsRenderable) {
  Message system_note;
  system_note.role = Role::System;
  system_note.content = {TextContent{"SWE-bench: cloning repository."}};

  EXPECT_FALSE(firmius::tui::ShouldHideMessageInTranscript(
      system_note, false, "system-note-123"));
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
  summon_view.name = "Delegate";
  summon_view.subagent_id = "child-agent";

  EXPECT_FALSE(firmius::tui::ShouldRenderFocusedSubagentToolCall(
      parent_entry, summon_view, "child-agent"));

  TimelineEntry child_entry;
  child_entry.kind = TimelineEntry::Kind::ToolCall;
  child_entry.id = "tool-child";
  child_entry.agentId = "child-agent";

  ToolCallView child_view;
  child_view.name = "Read";

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
      ToolCallContent{"exec-1", "Process", R"({"action":"Execute","command":"sleep 5"})"},
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
      ToolCallContent{"wait-1", "Process",
                      R"({"action":"Wait","process_id":"proc-1","pattern":"READY"})"},
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
  view->name = "Process";
  view->args = R"({"action":"Status","process_id":"proc-1"})";
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
      ToolCallContent{"status-1", "Process", R"({"action":"Status","process_id":"proc-1"})"},
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

TEST(ChatWindowHelpersTest, RebuildsHistoryWhenExistingTurnGainsContent) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {TextContent{"First transcript line."}};
  turn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(turn));

  auto chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; }, nullptr,
      [](const std::string &) -> std::shared_ptr<ToolCallView> {
        return nullptr;
      });

  const std::string initial = renderComponentToString(chat, 60, 8);
  EXPECT_NE(initial.find("First transcript line."), std::string::npos);

  history.turns[0].messages[0].content.push_back(
      TextContent{"Second transcript line added later in the same turn."});

  const std::string updated = renderComponentToString(chat, 60, 8);
  EXPECT_NE(updated.find("Second transcript line"), std::string::npos);
}


TEST(ChatWindowHelpersTest, RendersFullOversizedTranscriptBodiesWithoutClampBanner) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";

  std::string huge = "Large transcript heading\n";
  for (int i = 0; i < 6000; ++i) {
    huge += "body line " + std::to_string(i) + "\n";
  }

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {TextContent{huge}};
  turn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(turn));

  auto chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; }, nullptr,
      [](const std::string &) -> std::shared_ptr<ToolCallView> {
        return nullptr;
      });

  const std::string output = renderComponentToString(chat, 100, 24);
  EXPECT_FALSE(output.empty());
  EXPECT_EQ(output.find("UI preview truncated"), std::string::npos);
}

TEST(ChatWindowHelpersTest,
     HistorySummonSubagentUsesNormalizedStateFactsViaGetter) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";
  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {
      ToolCallContent{"summon-1", "Delegate",
                      R"({"action":"Spawn","title":"Worker","task":"Implement login"})"},
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
  view->name = "Delegate";
  view->args = R"({"action":"Wait","agent_id":"child-1"})";
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
      ToolCallContent{"wait-1", "Delegate", R"({"action":"Wait","agent_id":"child-1"})"},
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
      ToolCallContent{"read-1", "Read",
                      R"({"path":"src/main.cpp","start_line":1,"end_line":3})"},
      ToolCallContent{"ls-1", "List", R"({"path":"src"})"},
  };
  Message tool_result;
  tool_result.role = Role::ToolResult;
  tool_result.content = {
      ToolResultContent{"read-1",
                        R"({"line_start":1,"line_end":3,"lines_read":3,"watch_state":"updated","watch_scope":"range"})",
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
  EXPECT_NE(output.find("read"), std::string::npos);
  EXPECT_NE(output.find("src/main.cpp:1-3"), std::string::npos);
  EXPECT_NE(output.find("listed"), std::string::npos);
  EXPECT_NE(output.find("src"), std::string::npos);
}

TEST(ChatWindowHelpersTest, TurnFooterUsesCompactDoneSummary) {
  AgentHistory history;
  AgentTurn turn;
  turn.turnId = "turn-1";
  turn.metrics.timing.startMs = 1000;
  turn.metrics.timing.endMs = 6500;
  turn.metrics.context.sentTokens = 2400;
  turn.metrics.context.billedPromptTokens = 3000;
  turn.metrics.tokens.completion = 180;

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content = {TextContent{"Finished."}};
  turn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(turn));

  auto chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; }, nullptr,
      [](const std::string &) -> std::shared_ptr<ToolCallView> { return nullptr; });

  const std::string output = renderComponentToString(chat, 100, 12);
  EXPECT_NE(output.find("done"), std::string::npos);
  EXPECT_NE(output.find("turn 1"), std::string::npos);
  EXPECT_NE(output.find("5s"), std::string::npos);
  EXPECT_NE(output.find("↑2.4k/3k"), std::string::npos);
  EXPECT_NE(output.find("↓180"), std::string::npos);
}

} // namespace
