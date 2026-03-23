#include "ActivePlanState.hpp"
#include "ThemeManager.hpp"
#include "components/PlanLane.hpp"

#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace {

std::string renderComponentToString(const ftxui::Component &component,
                                    int width = 80, int height = 8) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  Render(screen, component->Render());
  Render(screen, component->Render());
  return screen.ToString();
}

TEST(PlanLaneTest, PlanLaneAlwaysShowsBodyAndStatusIcons) {
  auto model = std::make_shared<firmius::tui::PlanLaneModel>();
  model->visible = true;
  model->expanded = false;
  model->plan_title = "Transcript polish";
  model->chunks = {
      {"1", "Investigate", firmius::shared::WorkChunkStatus::Failed, "Failed", std::nullopt},
      {"2", "Patch", firmius::shared::WorkChunkStatus::Blocked, "Blocked", std::nullopt},
      {"3", "Retry", firmius::shared::WorkChunkStatus::Cancelled, "Cancelled", std::nullopt},
  };

  auto component = firmius::tui::PlanLane(model);
  auto output = renderComponentToString(component, 72, 10);

  EXPECT_NE(output.find("Investigate"), std::string::npos);
  EXPECT_NE(output.find("Patch"), std::string::npos);
  EXPECT_NE(output.find("Retry"), std::string::npos);
  EXPECT_NE(output.find(""), std::string::npos);
  EXPECT_NE(output.find(""), std::string::npos);
  EXPECT_NE(output.find(""), std::string::npos);
}

TEST(PlanLaneTest, ExpandedLaneWrapsLongChunkTitlesAndShowsTaskCounts) {
  auto model = std::make_shared<firmius::tui::PlanLaneModel>();
  model->visible = true;
  model->expanded = true;
  model->plan_title = "Transcript polish";
  model->chunks = {{
      "chunk-1",
      "Implement a very long chunk title that should wrap instead of truncating "
      "the plan lane body when rendered inside a narrow viewport",
      firmius::shared::WorkChunkStatus::Ready,
      "Ready",
      3,
  }};

  auto component = firmius::tui::PlanLane(model);
  auto output = renderComponentToString(component, 44, 8);

  EXPECT_NE(output.find("should wrap"), std::string::npos);
  EXPECT_NE(output.find("3 tasks"), std::string::npos);
  EXPECT_NE(output.find(""), std::string::npos);
}

TEST(PlanLaneTest, ExecutorTaskViewShowsChunkTasksOnly) {
  auto model = std::make_shared<firmius::tui::PlanLaneModel>();
  model->visible = true;
  model->expanded = false;
  model->plan_title = "Ignored in executor task view";
  model->executor_task_view = true;
  model->executor_chunk_title = "Runtime Support";
  model->executor_tasks = {
      {"task-1", "Implement print runtime call", firmius::shared::WorkChunkStatus::InProgress,
       "In Progress"},
      {"task-2", "Wire class alloc helper", firmius::shared::WorkChunkStatus::Ready,
       "Ready"},
  };

  auto component = firmius::tui::PlanLane(model);
  auto output = renderComponentToString(component, 48, 10);

  EXPECT_NE(output.find("Runtime Support"), std::string::npos);
  EXPECT_NE(output.find("Implement print"), std::string::npos);
  EXPECT_NE(output.find("Wire class alloc"), std::string::npos);
  EXPECT_EQ(output.find("PLAN"), std::string::npos);
}

TEST(PlanLaneTest, ExecutorFlatChunkViewHighlightsAssignedChunk) {
  auto model = std::make_shared<firmius::tui::PlanLaneModel>();
  model->visible = true;
  model->expanded = true;
  model->plan_title = "Compiler Plan";
  model->highlight_chunk_id = "chunk-2";
  model->chunks = {
      {"chunk-1", "Lexer", firmius::shared::WorkChunkStatus::Done, "Done", std::nullopt},
      {"chunk-2", "Runtime Support", firmius::shared::WorkChunkStatus::InProgress,
       "In Progress", std::nullopt},
  };

  auto component = firmius::tui::PlanLane(model);
  auto output = renderComponentToString(component, 56, 10);

  EXPECT_NE(output.find("Runtime Support"), std::string::npos);
  EXPECT_NE(output.find("󰎤"), std::string::npos);
}

} // namespace
