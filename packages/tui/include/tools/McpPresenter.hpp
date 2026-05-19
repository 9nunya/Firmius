#ifndef FIRMIUS_TUI_MCPPRESENTER_HPP
#define FIRMIUS_TUI_MCPPRESENTER_HPP

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Presenter for MCP tools (mcp__<server>__<tool>).
class McpPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Mcp"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_MCPPRESENTER_HPP
