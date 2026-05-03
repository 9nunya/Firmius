#include "models/PermissionModel.hpp"

namespace firmius::tui {

PermissionModel& PermissionModel::instance() {
  static PermissionModel inst;
  return inst;
}

} // namespace firmius::tui
