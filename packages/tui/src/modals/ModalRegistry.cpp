#include "modals/ModalRegistry.hpp"
#include "TUIState.hpp"

namespace firmius::tui {

ModalRegistry &ModalRegistry::instance() {
  static ModalRegistry inst;
  return inst;
}

void ModalRegistry::registerModal(std::shared_ptr<IModal> modal) {
  modals_[modal->name()] = std::move(modal);
}

bool ModalRegistry::openModal(const std::string &name, TuiState &state) {
  auto it = modals_.find(name);
  if (it == modals_.end()) {
    return false;
  }

  auto component = it->second->create(state);
  state.openModalDirect(component);
  return true;
}

} // namespace firmius::tui
