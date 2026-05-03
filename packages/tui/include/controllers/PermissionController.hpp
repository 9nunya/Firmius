#ifndef FIRMIUS_TUI_CONTROLLERS_PERMISSION_CONTROLLER_HPP
#define FIRMIUS_TUI_CONTROLLERS_PERMISSION_CONTROLLER_HPP

#include "models/PermissionModel.hpp"
#include "Events.hpp"

namespace firmius::tui {

class PermissionController {
public:
  static PermissionController& instance();
  void activateRequest(const firmius::shared::PermissionEscalationRequest& request);
  void promoteNextRequest();

private:
  PermissionController() = default;
};

} // namespace firmius::tui

#endif
