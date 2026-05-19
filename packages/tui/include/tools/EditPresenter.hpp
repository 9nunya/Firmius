#ifndef FIRMIUS_TUI_EDITPRESENTER_HPP
#define FIRMIUS_TUI_EDITPRESENTER_HPP

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Presenter for Edit, EditWrite, EditReplace, EditRange (all diff-based).
class EditPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Edit"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_EDITPRESENTER_HPP
