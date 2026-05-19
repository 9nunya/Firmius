#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Presenter for Grep tool.
class GrepPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Grep"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui
