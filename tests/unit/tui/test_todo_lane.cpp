#include "components/TodoLane.hpp"

#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace {

std::string renderComponentToString(const ftxui::Component &component,
                                    int width = 100, int height = 10) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  Render(screen, component->Render());
  Render(screen, component->Render());
  return screen.ToString();
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

} // namespace
