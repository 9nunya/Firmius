#include "components/ChatWindow.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::shared::AgentHistory;
using firmius::shared::AgentTurn;
using firmius::shared::Message;
using firmius::shared::Role;
using firmius::shared::TextContent;
using firmius::shared::ToolCallContent;
using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

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

} // namespace
