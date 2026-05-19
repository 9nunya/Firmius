#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Presenter for List tool.
class ListPresenter : public IToolPresenter {
public:
  std::string name() const override { return "List"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui
