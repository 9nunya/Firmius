#include "WorkPanelLayout.hpp"

#include <algorithm>

namespace firmius::tui {

int computeWorkPanelMaxHeight(int terminalHeight) {
  // Keep the lanes compact, but leave one extra line so mid-sized terminals
  // don't feel cramped once the lane header is accounted for.
  return std::clamp(1 + terminalHeight / 14, 5, 10);
}

std::vector<WorkPanelTab> availableWorkPanelTabs(bool hasPlan, bool hasTodo,
                                                 bool hasContext) {
  std::vector<WorkPanelTab> tabs;
  if (hasTodo) {
    tabs.push_back(WorkPanelTab::Todo);
  }
  if (hasPlan) {
    tabs.push_back(WorkPanelTab::Plan);
  }
  if (hasContext) {
    tabs.push_back(WorkPanelTab::Context);
  }
  return tabs;
}

WorkPanelTab normalizeWorkPanelTab(WorkPanelTab preferred, bool hasPlan,
                                   bool hasTodo, bool hasContext) {
  const auto tabs = availableWorkPanelTabs(hasPlan, hasTodo, hasContext);
  if (tabs.empty()) {
    return WorkPanelTab::Context;
  }
  for (const auto tab : tabs) {
    if (tab == preferred) {
      return preferred;
    }
  }
  if (hasContext) {
    return WorkPanelTab::Context;
  }
  return tabs.front();
}

WorkPanelTab nextWorkPanelTab(WorkPanelTab current, bool hasPlan, bool hasTodo,
                              bool hasContext) {
  const auto tabs = availableWorkPanelTabs(hasPlan, hasTodo, hasContext);
  if (tabs.empty()) {
    return WorkPanelTab::Context;
  }
  const auto it = std::find(tabs.begin(), tabs.end(), current);
  if (it == tabs.end()) {
    return tabs.front();
  }
  const auto next_index =
      static_cast<std::size_t>(std::distance(tabs.begin(), it) + 1) %
      tabs.size();
  return tabs[next_index];
}

WorkPanelDecision determineWorkPanelDecision(bool isLead, bool isExecutor,
                                             bool hasPlan, bool hasTodo,
                                             bool hasExecutorChunk,
                                             int terminalWidth,
                                             int terminalHeight,
                                             bool preferTodoOnNarrow) {
  WorkPanelDecision decision;

  if (!hasPlan && !hasTodo) {
    return decision;
  }

  if (isExecutor && hasTodo && hasExecutorChunk) {
    decision.kind = WorkPanelKind::ExecutorChunkTodo;
    decision.showTodo = true;
    return decision;
  }

  if (hasPlan && hasTodo) {
    const bool canSplit = terminalWidth >= 140 && terminalHeight >= 20;
    if (canSplit) {
      decision.kind = WorkPanelKind::SplitPlanTodo;
      decision.showPlan = true;
      decision.showTodo = true;
      return decision;
    }
    decision.kind = WorkPanelKind::SingleToggle;
    decision.showToggleLabel = true;
    decision.activePanelLabel = preferTodoOnNarrow ? "TODO" : "PLAN";
    decision.showTodo = preferTodoOnNarrow;
    decision.showPlan = !preferTodoOnNarrow;
    return decision;
  }

  if (hasTodo) {
    decision.kind = WorkPanelKind::TodoOnly;
    decision.showTodo = true;
    return decision;
  }

  if (hasPlan && isLead) {
    decision.kind = WorkPanelKind::PlanOnly;
    decision.showPlan = true;
    return decision;
  }

  if (hasPlan) {
    decision.kind = WorkPanelKind::PlanOnly;
    decision.showPlan = true;
  }

  return decision;
}

} // namespace firmius::tui
