#include "models/WorkspaceModel.hpp"

namespace firmius::tui {

WorkspaceModel& WorkspaceModel::instance() {
  static WorkspaceModel inst;
  return inst;
}

} // namespace firmius::tui
