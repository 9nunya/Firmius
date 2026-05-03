#ifndef FIRMIUS_TUI_PERMISSION_MODEL_HPP
#define FIRMIUS_TUI_PERMISSION_MODEL_HPP

#include "Events.hpp"
#include <ftxui/screen/box.hpp>
#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <optional>

namespace firmius::tui {

class PermissionModel {
public:
  static PermissionModel& instance();

  std::optional<firmius::shared::PermissionEscalationRequest> pending_request;
  std::vector<firmius::shared::PermissionEscalationRequest> request_queue;
  
  std::vector<std::string> active_labels;
  std::vector<firmius::shared::PermissionResponse> active_responses;
  std::vector<ftxui::Box> active_option_boxes;
  int selected_index = 0;

  std::function<void()> on_request_activated;
  std::function<void()> on_request_cleared;

private:
  PermissionModel() = default;
};

} // namespace firmius::tui

#endif
