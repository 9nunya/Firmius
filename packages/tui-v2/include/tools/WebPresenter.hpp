#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui2 {

/// Presenter for Web tool (Search, Fetch).
class WebPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Web"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui2
