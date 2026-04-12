#ifndef FIRMIUS_WORK_PANEL_LAYOUT_HPP
#define FIRMIUS_WORK_PANEL_LAYOUT_HPP

#include <string>
#include <vector>

namespace firmius::tui {

enum class WorkPanelKind {
  None,
  PlanOnly,
  TodoOnly,
  ContextOnly,
  SplitPlanTodo,
  SingleToggle,
  ExecutorChunkTodo
};

enum class WorkPanelTab {
  Plan,
  Todo,
  Context,
};

struct WorkPanelDecision {
  WorkPanelKind kind = WorkPanelKind::None;
  bool showPlan = false;
  bool showTodo = false;
  bool showToggleLabel = false;
  std::string activePanelLabel;
};

WorkPanelDecision determineWorkPanelDecision(bool isLead, bool isExecutor,
                                             bool hasPlan, bool hasTodo,
                                             bool hasExecutorChunk,
                                             int terminalWidth,
                                             int terminalHeight,
                                             bool preferTodoOnNarrow);

int computeWorkPanelMaxHeight(int terminalHeight);

std::vector<WorkPanelTab> availableWorkPanelTabs(bool hasPlan, bool hasTodo,
                                                 bool hasContext);
WorkPanelTab normalizeWorkPanelTab(WorkPanelTab preferred, bool hasPlan,
                                   bool hasTodo, bool hasContext);
WorkPanelTab nextWorkPanelTab(WorkPanelTab current, bool hasPlan, bool hasTodo,
                              bool hasContext);

} // namespace firmius::tui

#endif
