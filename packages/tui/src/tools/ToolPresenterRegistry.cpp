#include "tools/ToolPresenterRegistry.hpp"

namespace firmius::tui {

ToolPresenterRegistry& ToolPresenterRegistry::instance() {
  static ToolPresenterRegistry registry;
  return registry;
}

void ToolPresenterRegistry::registerPresenter(std::unique_ptr<IToolPresenter> presenter) {
  presenters_.push_back(std::move(presenter));
}

const IToolPresenter* ToolPresenterRegistry::find(const std::string& toolName) const {
  for (const auto& presenter : presenters_) {
    if (presenter->matches(toolName)) {
      return presenter.get();
    }
  }
  return nullptr;
}

} // namespace firmius::tui
