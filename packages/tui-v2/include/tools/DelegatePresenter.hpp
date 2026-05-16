#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui2 {

/// Presenter for Delegate tool (Spawn, Wait, Stop).
class DelegatePresenter : public IToolPresenter {
public:
  std::string name() const override { return "Delegate"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, int width) const override;
};

} // namespace firmius::tui2
