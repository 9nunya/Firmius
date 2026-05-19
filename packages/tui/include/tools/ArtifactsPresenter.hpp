#ifndef FIRMIUS_TUI_ARTIFACTSPRESENTER_HPP
#define FIRMIUS_TUI_ARTIFACTSPRESENTER_HPP

#include "IToolPresenter.hpp"

namespace firmius::tui {

/// Presenter for Artifacts tool (Write, Read, List).
class ArtifactsPresenter : public IToolPresenter {
public:
  std::string name() const override { return "Artifacts"; }
  bool matches(const std::string& toolName) const override;
  std::vector<std::string> render(const ToolCallItem& item, const ToolRenderContext& ctx, int width) const override;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_ARTIFACTSPRESENTER_HPP
