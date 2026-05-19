#ifndef FIRMIUS_TUI_DELEGATEPRESENTER_HPP
#define FIRMIUS_TUI_DELEGATEPRESENTER_HPP

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Presenter for Delegate tool (Spawn, Wait, Stop).
class DelegatePresenter : public IToolPresenter {
public:
  std::string name() const override { return "Delegate"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_DELEGATEPRESENTER_HPP
