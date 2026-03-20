#include "WorkPanelLayout.hpp"

namespace firmius::tui {

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
