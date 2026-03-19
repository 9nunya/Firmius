#include "ActivePlanState.hpp"
#include "ThemeManager.hpp"
#include "components/PlanLane.hpp"

#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace {

std::string renderToString(ftxui::Element element, int width = 80, int height = 8) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  Render(screen, element);
  return screen.ToString();
}

TEST(PlanLaneTest, CollapsedLaneSurfacesBadStates) {
  auto model = std::make_shared<firmius::tui::PlanLaneModel>();
  model->visible = true;
  model->expanded = false;
  model->plan_title = "Transcript polish";
  model->chunks = {
      {"1", "Investigate", firmius::shared::WorkChunkStatus::Failed, "Failed"},
      {"2", "Patch", firmius::shared::WorkChunkStatus::Blocked, "Blocked"},
      {"3", "Retry", firmius::shared::WorkChunkStatus::Cancelled, "Cancelled"},
  };

  auto component = firmius::tui::PlanLane(model);
  auto output = renderToString(component->Render());

  EXPECT_NE(output.find("failed"), std::string::npos);
  EXPECT_NE(output.find("blocked"), std::string::npos);
  EXPECT_NE(output.find("cancelled"), std::string::npos);
}

} // namespace
