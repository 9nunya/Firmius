#pragma once

#include "IToolPresenter.hpp"

namespace firmius::tui2 {

/// Presenter for Artifacts tool (Write, Read, List).
class ArtifactsPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Artifacts"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, int width) const override;
};

} // namespace firmius::tui2
