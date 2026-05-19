#ifndef FIRMIUS_TUI_LSPPRESENTER_HPP
#define FIRMIUS_TUI_LSPPRESENTER_HPP

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Presenter for Lsp tool (Query, Diagnostics).
class LspPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Lsp"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_LSPPRESENTER_HPP
