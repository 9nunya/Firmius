#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui2 {

/// Generic fallback presenter. Matches any tool name.
class GenericPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Generic"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, int width) const override;
};

} // namespace firmius::tui2
