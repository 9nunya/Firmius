#pragma once

#include "IToolPresenter.hpp"

#include <memory>
#include <string>
#include <vector>

namespace firmius::tui2 {

/// Registry that dispatches tool rendering to the appropriate presenter.
class ToolPresenterRegistry {
public:
  static ToolPresenterRegistry& instance();

  /// Register a presenter. Order matters — first match wins.
  void registerPresenter(std::unique_ptr<IToolPresenter> presenter);

  /// Find the presenter for a given tool name.
  /// Tries specialized presenters first, then generic fallback.
  const IToolPresenter* find(const std::string& toolName) const;

private:
  ToolPresenterRegistry() = default;
  std::vector<std::unique_ptr<IToolPresenter>> presenters_;
};

} // namespace firmius::tui2
