#ifndef FIRMIUS_TUI_GENERICPRESENTER_HPP
#define FIRMIUS_TUI_GENERICPRESENTER_HPP

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Generic fallback presenter. Matches any tool name.
class GenericPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Generic"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_GENERICPRESENTER_HPP
