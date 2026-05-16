#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui2 {

/// Presenter for MCP tools (mcp__<server>__<tool>).
class McpPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Mcp"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, int width) const override;
};

} // namespace firmius::tui2
