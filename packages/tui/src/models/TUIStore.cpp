#include "models/TUIStore.hpp"

namespace firmius::tui {

TUIStore& TUIStore::instance() {
  static TUIStore inst;
  return inst;
}

TUIStore::TUIStore() {
  title_model = std::make_shared<TitleBarModel>();
  status_model = std::make_shared<StatusBarModel>();
  input_model = std::make_shared<InputBarModel>();
  input_model->buffer = &input_buffer;
  input_model->cursor = &input_cursor;
  agent_strip_model = std::make_shared<AgentStripModel>();
  plan_lane_model = std::make_shared<PlanLaneModel>();
  todo_lane_model = std::make_shared<TodoLaneModel>();
  context_lane_model = std::make_shared<ContextLaneModel>();
}

} // namespace firmius::tui
