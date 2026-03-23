#ifndef FIRMIUS_WORK_PANEL_LAYOUT_HPP
#define FIRMIUS_WORK_PANEL_LAYOUT_HPP

#include <string>

namespace firmius::tui {

enum class WorkPanelKind {
  None,
  PlanOnly,
  TodoOnly,
  SplitPlanTodo,
  SingleToggle,
  ExecutorChunkTodo
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

} // namespace firmius::tui

#endif
