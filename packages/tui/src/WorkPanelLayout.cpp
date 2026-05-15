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
  (void)hasPlan;
  std::vector<WorkPanelTab> tabs;
  if (hasTodo) {
    tabs.push_back(WorkPanelTab::Todo);
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
  (void)isLead;
  (void)hasPlan;
  (void)terminalWidth;
  (void)terminalHeight;
  (void)preferTodoOnNarrow;
  WorkPanelDecision decision;

  if (!hasTodo) {
    return decision;
  }

  if (isExecutor && hasTodo && hasExecutorChunk) {
    decision.kind = WorkPanelKind::ExecutorChunkTodo;
    decision.showTodo = true;
    return decision;
  }

  if (hasTodo) {
    decision.kind = WorkPanelKind::TodoOnly;
    decision.showTodo = true;
    return decision;
  }

  return decision;
}

} // namespace firmius::tui
