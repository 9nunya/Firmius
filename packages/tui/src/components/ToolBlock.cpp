#include "components/ToolBlock.hpp"
#include "components/ToolPresentationBlock.hpp"
#include "tools/ToolPresentation.hpp"
#include <ftxui/component/component.hpp>

namespace firmius::tui {

ftxui::Component ToolBlock(const std::shared_ptr<ToolCallView> &view,
                           HistoryGetter history_getter,
                           StreamGetter stream_getter,
                           ProcessStateGetter process_state_getter,
                           SubagentStateGetter subagent_state_getter) {
  if (!view)
    return ftxui::Renderer([] { return ftxui::text("Missing tool view"); });

  (void)history_getter;
  (void)stream_getter;

  return ToolPresentationBlock(view, [view, process_state_getter,
                                      subagent_state_getter] {
    const NormalizedProcessState *process_state = nullptr;
    const NormalizedSubagentState *subagent_state = nullptr;
    if (process_state_getter && view) {
      process_state = process_state_getter(view->toolCallId);
    }
    if (subagent_state_getter && view) {
      subagent_state = subagent_state_getter(view->toolCallId);
    }
    return BuildToolPresentation(*view, process_state, subagent_state);
  });
}

} // namespace firmius::tui
