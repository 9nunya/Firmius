#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Presenter for Glob tool.
class GlobPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Glob"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui
