#include "WorkPanelLayout.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::tui::WorkPanelKind;
using firmius::tui::WorkPanelTab;
using firmius::tui::availableWorkPanelTabs;
using firmius::tui::computeWorkPanelMaxHeight;
using firmius::tui::determineWorkPanelDecision;
using firmius::tui::nextWorkPanelTab;
using firmius::tui::normalizeWorkPanelTab;

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

TEST(WorkPanelLayoutTest, WorkPanelHeightStaysCompactOnNormalTerminals) {
  EXPECT_EQ(computeWorkPanelMaxHeight(32), 5);
  EXPECT_EQ(computeWorkPanelMaxHeight(56), 5);
  EXPECT_EQ(computeWorkPanelMaxHeight(84), 7);
  EXPECT_EQ(computeWorkPanelMaxHeight(200), 10);
}

TEST(WorkPanelLayoutTest, AvailableTabsIncludeContextWhenVisible) {
  const auto tabs = availableWorkPanelTabs(true, true, true);
  ASSERT_EQ(tabs.size(), 3u);
  EXPECT_EQ(tabs[0], WorkPanelTab::Todo);
  EXPECT_EQ(tabs[1], WorkPanelTab::Plan);
  EXPECT_EQ(tabs[2], WorkPanelTab::Context);
}

TEST(WorkPanelLayoutTest, NormalizeTabFallsBackToContextThenFirstAvailable) {
  EXPECT_EQ(normalizeWorkPanelTab(WorkPanelTab::Plan, false, false, true),
            WorkPanelTab::Context);
  EXPECT_EQ(normalizeWorkPanelTab(WorkPanelTab::Context, false, true, false),
            WorkPanelTab::Todo);
}

TEST(WorkPanelLayoutTest, NextTabCyclesAcrossVisibleTabs) {
  EXPECT_EQ(nextWorkPanelTab(WorkPanelTab::Todo, true, true, true),
            WorkPanelTab::Plan);
  EXPECT_EQ(nextWorkPanelTab(WorkPanelTab::Plan, true, true, true),
            WorkPanelTab::Context);
  EXPECT_EQ(nextWorkPanelTab(WorkPanelTab::Context, true, true, true),
            WorkPanelTab::Todo);
}

} // namespace
