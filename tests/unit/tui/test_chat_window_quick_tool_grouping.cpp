#include "components/ChatWindow.hpp"

#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace {

using firmius::shared::AgentHistory;
using firmius::shared::AgentTurn;
using firmius::shared::Message;
using firmius::shared::Role;
using firmius::shared::ToolCallContent;
using firmius::shared::ToolResultContent;

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

TEST(ChatWindowQuickToolGroupingTest, GroupsConsecutiveToolOnlyTurnsAcrossFooters) {
  AgentHistory history;

  // Turn 1: tool-only assistant message (read)
  {
    AgentTurn turn;
    turn.turnId = "turn-1";

    Message assistant;
    assistant.role = Role::Assistant;
    assistant.content = {
        ToolCallContent{"read-1", "Read",
                        R"({"path":"a.txt"})"},
    };
    turn.messages.push_back(std::move(assistant));

    turn.metrics.timing.startMs = 1000;
    turn.metrics.timing.endMs = 2000;

    // Match the tool call with a tool result so the view renders as Finished.
    Message tool_result;
    tool_result.role = Role::ToolResult;
    tool_result.content = {
        ToolResultContent{"read-1", "{}", true, "", ""},
    };
    turn.messages.push_back(std::move(tool_result));

    history.turns.push_back(std::move(turn));
  }

  // Turn 2: tool-only assistant message (read)
  {
    AgentTurn turn;
    turn.turnId = "turn-2";

    Message assistant;
    assistant.role = Role::Assistant;
    assistant.content = {
        ToolCallContent{"read-2", "Read",
                        R"({"path":"b.txt"})"},
    };
    turn.messages.push_back(std::move(assistant));

    turn.metrics.timing.startMs = 2000;
    turn.metrics.timing.endMs = 3000;

    Message tool_result;
    tool_result.role = Role::ToolResult;
    tool_result.content = {
        ToolResultContent{"read-2", "{}", true, "", ""},
    };
    turn.messages.push_back(std::move(tool_result));

    history.turns.push_back(std::move(turn));
  }

  // Turn 3: tool-only assistant message (read)
  {
    AgentTurn turn;
    turn.turnId = "turn-3";

    Message assistant;
    assistant.role = Role::Assistant;
    assistant.content = {
        ToolCallContent{"read-3", "Read",
                        R"({"path":"c.txt"})"},
    };
    turn.messages.push_back(std::move(assistant));

    turn.metrics.timing.startMs = 3000;
    turn.metrics.timing.endMs = 4000;

    Message tool_result;
    tool_result.role = Role::ToolResult;
    tool_result.content = {
        ToolResultContent{"read-3", "{}", true, "", ""},
    };
    turn.messages.push_back(std::move(tool_result));

    history.turns.push_back(std::move(turn));
  }

  auto chat = firmius::tui::ChatWindow(
      [&history]() -> const AgentHistory * { return &history; }, nullptr,
      [](const std::string &) -> std::shared_ptr<firmius::shared::ToolCallView> {
        return nullptr;
      });

  const std::string output = renderComponentToString(chat, 140, 24);

  // Both targets should be present on the same grouped row.
  EXPECT_NE(output.find("a.txt, b.txt, c.txt"), std::string::npos);
  const auto grouped_pos = output.find("a.txt, b.txt, c.txt");
  ASSERT_NE(grouped_pos, std::string::npos);


  // Footers should not stack once per quick-tool-only turn when those turns are
  const std::string footer_snippet = " done · turn ";
  // consumed into a single grouped quick-tools block.
  size_t footer_count = 0;
  for (size_t pos = output.find(footer_snippet); pos != std::string::npos;
       pos = output.find(footer_snippet, pos + 1)) {
    ++footer_count;
    EXPECT_GT(pos, grouped_pos);
  }
  EXPECT_LE(footer_count, 1u);
}

} // namespace
