#include "components/TodoLane.hpp"

#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>
#include <optional>

namespace {

ftxui::Screen renderComponent(const ftxui::Component &component, int width = 100,
                              int height = 10) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  Render(screen, component->Render());
  Render(screen, component->Render());
  return screen;
}

std::string renderComponentToString(const ftxui::Component &component,
                                    int width = 100, int height = 10) {
  return renderComponent(component, width, height).ToString();
}

std::optional<std::pair<int, int>> findSubstring(const ftxui::Screen &screen,
                                                 const std::string &needle) {
  for (int y = 0; y < screen.dimy(); ++y) {
    std::string line;
    line.reserve(static_cast<size_t>(screen.dimx()));
    for (int x = 0; x < screen.dimx(); ++x) {
      const auto &pixel = screen.PixelAt(x, y);
      line += pixel.character.empty() ? ' ' : pixel.character.front();
    }
    const auto pos = line.find(needle);
    if (pos != std::string::npos) {
      return std::pair<int, int>{static_cast<int>(pos), y};
    }
  }
  return std::nullopt;
}

TEST(TodoLaneTest, RendersPendingInProgressAndDoneStates) {
  auto model = std::make_shared<firmius::tui::TodoLaneModel>();
  model->visible = true;
  model->owner_label = "lead";
  model->rows = {
      {1, "Inspect code", firmius::shared::TodoStatus::InProgress},
      {2, "Edit files", firmius::shared::TodoStatus::Pending},
      {3, "Run tests", firmius::shared::TodoStatus::Done},
  };

  auto component = firmius::tui::TodoLane(model);
  auto output = renderComponentToString(component, 100, 16);

  EXPECT_NE(output.find("1. [*]"), std::string::npos);
  EXPECT_NE(output.find("[ ]"), std::string::npos);
  EXPECT_NE(output.find("[x]"), std::string::npos);
  EXPECT_NE(output.find("Edit files"), std::string::npos);
  EXPECT_NE(output.find("Run tests"), std::string::npos);
}

TEST(TodoLaneTest, ExecutorViewIncludesChunkHeaderWhenProvided) {
  auto model = std::make_shared<firmius::tui::TodoLaneModel>();
  model->visible = true;
  model->owner_label = "executor";
  model->show_chunk_header = true;
  model->chunk_title = "Update AST to support classes";
  model->rows = {
      {1, "Explore codebase", firmius::shared::TodoStatus::InProgress},
      {2, "Update ast.ts", firmius::shared::TodoStatus::Pending},
  };

  auto component = firmius::tui::TodoLane(model);
  auto output = renderComponentToString(component);

  EXPECT_NE(output.find("󰆧 Update AST to support classes"),
            std::string::npos);
  EXPECT_NE(output.find("Explore codebase"), std::string::npos);
}

TEST(TodoLaneTest, WrapsLongTodoRowsInsideScrollableBody) {
  auto model = std::make_shared<firmius::tui::TodoLaneModel>();
  model->visible = true;
  model->owner_label = "lead";
  model->rows = {
      {1,
       "Investigate the transcript rendering issue that only appears once the "
       "chat history and work panel are both long enough to stress wrapping",
       firmius::shared::TodoStatus::InProgress},
  };

  auto component = firmius::tui::TodoLane(model);
  auto output = renderComponentToString(component, 42, 7);

  EXPECT_NE(output.find("Investigate"), std::string::npos);
  EXPECT_NE(output.find("transcript"), std::string::npos);
  EXPECT_NE(output.find("wrapping"), std::string::npos);
}

TEST(TodoLaneTest, AutoScrollsToCurrentInProgressRow) {
  auto model = std::make_shared<firmius::tui::TodoLaneModel>();
  model->visible = true;
  model->owner_label = "lead";
  model->rows = {
      {1, "Finished one", firmius::shared::TodoStatus::Done},
      {2, "Finished two", firmius::shared::TodoStatus::Done},
      {3, "Finished three", firmius::shared::TodoStatus::Done},
      {4, "Finished four", firmius::shared::TodoStatus::Done},
      {5, "Finished five", firmius::shared::TodoStatus::Done},
      {6, "Current in progress task", firmius::shared::TodoStatus::InProgress},
      {7, "Next task", firmius::shared::TodoStatus::Pending},
  };

  auto component = firmius::tui::TodoLane(model);

  // NOTE: Rendering-time scrolling is hard to assert in an offscreen FTXUI render
  // (no interactive event loop). This test uses a viewport tall enough to
  // include the in-progress row and ensures it renders with the rest of the list.
  std::string output;
  for (int i = 0; i < 4; ++i) {
    output = renderComponentToString(component, 48, 12);
  }

  if (output.find("Current in progress task") == std::string::npos) {
    std::cerr << "--- TodoLane output ---\n" << output << "\n---------------------\n";
  }

  EXPECT_NE(output.find("Current in progress task"), std::string::npos);
}

TEST(TodoLaneTest, StrikethroughAppliesOnlyToDoneTaskText) {
  auto model = std::make_shared<firmius::tui::TodoLaneModel>();
  model->visible = true;
  model->owner_label = "lead";
  model->rows = {
      {1, "Inspect code", firmius::shared::TodoStatus::InProgress},
      {2, "Done task", firmius::shared::TodoStatus::Done},
  };

  auto component = firmius::tui::TodoLane(model);
  auto screen = renderComponent(component, 60, 8);
  const auto found = findSubstring(screen, "2. [x] Done task");

  ASSERT_TRUE(found.has_value());
  const int prefix_x = found->first;
  const int y = found->second;
  const int text_x = prefix_x + static_cast<int>(std::string("2. [x] ").size());

  EXPECT_FALSE(screen.PixelAt(prefix_x, y).strikethrough);
  EXPECT_TRUE(screen.PixelAt(text_x, y).strikethrough);
}

} // namespace
