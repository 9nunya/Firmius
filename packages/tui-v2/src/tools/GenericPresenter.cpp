#include "tools/GenericPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"

namespace firmius::tui2 {

bool GenericPresenter::matches(const std::string& /*toolName*/) const {
  return true;  // catches everything
}

std::vector<std::string> GenericPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 " + item.toolName())};
  }

  if (item.phase() == ToolPhase::Called) {
    return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 " + item.toolName())};
  }

  if (item.success()) {
    return {ansi::fgRgb(100, 200, 120, "  \xe2\x9c\x93 " + item.toolName() + " \xe2\x80\x94 done")};
  }
  return {ansi::fgRgb(220, 80, 80, "  \xe2\x9c\x97 " + item.toolName() + " \xe2\x80\x94 failed")};
}

} // namespace firmius::tui2
