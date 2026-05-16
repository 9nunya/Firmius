#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui2 {

/// Presenter for Process (8 actions) and Python tools.
class ProcessPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Process"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, int width) const override;
};

} // namespace firmius::tui2
