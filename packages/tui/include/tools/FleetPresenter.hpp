#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Presenter for Fleet tool (Lock, Respond, Status).
class FleetPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Fleet"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui
