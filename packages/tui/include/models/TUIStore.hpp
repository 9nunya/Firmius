#ifndef FIRMIUS_TUI_STORE_HPP
#define FIRMIUS_TUI_STORE_HPP

#include "Context.hpp"
#include "SkinConfig.hpp"
#include "components/AgentStrip.hpp"
#include "components/PlanLane.hpp"
#include "components/TodoLane.hpp"
#include "components/ContextLane.hpp"
#include "components/StatusBar.hpp"
#include "components/TitleBar.hpp"
#include "components/InputBar.hpp"
#include <memory>
#include <string>
#include <functional>
#include <vector>

namespace firmius::tui {

class TUIStore {
public:
  static TUIStore& instance();

  std::string thread_id;
  std::string focused_agent_id;
  firmius::shared::ThreadMetadata thread_metadata;
  
  enum class ViewMode { Welcome, Chat, ProcessFocus };
  ViewMode view_mode = ViewMode::Welcome;

  SkinKind skin_kind = SkinKind::Firmius;
  SkinConfig skin_config = defaultSkinConfig(SkinKind::Firmius);

  std::shared_ptr<TitleBarModel> title_model;
  std::shared_ptr<StatusBarModel> status_model;
  std::shared_ptr<InputBarModel> input_model;
  std::shared_ptr<AgentStripModel> agent_strip_model;
  std::shared_ptr<PlanLaneModel> plan_lane_model;
  std::shared_ptr<TodoLaneModel> todo_lane_model;
  std::shared_ptr<ContextLaneModel> context_lane_model;
  
  std::string input_buffer;
  int input_cursor = 0;

  std::function<void()> on_identity_changed;
  std::function<void()> on_view_mode_changed;

private:
  TUIStore();
};

} // namespace firmius::tui

#endif
