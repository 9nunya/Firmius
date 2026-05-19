#ifndef FIRMIUS_TUI_READPRESENTER_HPP
#define FIRMIUS_TUI_READPRESENTER_HPP

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Presenter for Read tool.
class ReadPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Read"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_READPRESENTER_HPP
