#include "tools/GenericPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

namespace firmius::tui2 {

bool GenericPresenter::matches(const std::string& /*toolName*/) const {
  return true;  // catches everything
}

std::vector<std::string> GenericPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 " + item.toolName())};
  }

  if (item.phase() == ToolPhase::Called) {
    return {theme_ansi::warning("  \xe2\x9a\x99 " + item.toolName())};
  }

  if (item.success()) {
    return {theme_ansi::success("  \xe2\x9c\x93 " + item.toolName() + " \xe2\x80\x94 done")};
  }
  return {theme_ansi::error("  \xe2\x9c\x97 " + item.toolName() + " \xe2\x80\x94 failed")};
}

} // namespace firmius::tui2
