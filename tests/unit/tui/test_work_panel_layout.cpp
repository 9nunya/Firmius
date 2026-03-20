#include "WorkPanelLayout.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::tui::WorkPanelKind;
using firmius::tui::determineWorkPanelDecision;

TEST(WorkPanelLayoutTest, LeadGetsSplitPaneOnWideTerminalWhenPlanAndTodoExist) {
  auto decision = determineWorkPanelDecision(
      true, false, true, true, false, 180, 40, true);
  EXPECT_EQ(decision.kind, WorkPanelKind::SplitPlanTodo);
  EXPECT_TRUE(decision.showPlan);
  EXPECT_TRUE(decision.showTodo);
}

TEST(WorkPanelLayoutTest, LeadGetsTogglePanelOnNarrowTerminal) {
  auto decision = determineWorkPanelDecision(
      true, false, true, true, false, 100, 30, false);
  EXPECT_EQ(decision.kind, WorkPanelKind::SingleToggle);
  EXPECT_TRUE(decision.showPlan);
  EXPECT_FALSE(decision.showTodo);
  EXPECT_EQ(decision.activePanelLabel, "PLAN");
}

TEST(WorkPanelLayoutTest, ExecutorChunkTodoTakesPriority) {
  auto decision = determineWorkPanelDecision(
      false, true, true, true, true, 90, 20, true);
  EXPECT_EQ(decision.kind, WorkPanelKind::ExecutorChunkTodo);
  EXPECT_FALSE(decision.showPlan);
  EXPECT_TRUE(decision.showTodo);
}

} // namespace
