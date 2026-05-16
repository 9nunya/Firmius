#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui2 {

/// Presenter for Skill tool.
class SkillPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Skill"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui2
