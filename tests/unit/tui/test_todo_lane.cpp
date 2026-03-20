#include "components/TodoLane.hpp"

#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace {

std::string renderToString(ftxui::Element element, int width = 100,
                           int height = 10) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  Render(screen, element);
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
  auto output = renderToString(component->Render());

  EXPECT_NE(output.find("1. [*]"), std::string::npos);
  EXPECT_NE(output.find("2. [ ]"), std::string::npos);
  EXPECT_NE(output.find("3. [x]"), std::string::npos);
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
  auto output = renderToString(component->Render());

  EXPECT_NE(output.find("|CHUNK| Update AST to support classes"),
            std::string::npos);
  EXPECT_NE(output.find("Explore codebase"), std::string::npos);
}

} // namespace
