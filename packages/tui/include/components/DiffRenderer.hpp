#ifndef FIRMIUS_TUI_COMPONENTS_DIFF_RENDERER_HPP
#define FIRMIUS_TUI_COMPONENTS_DIFF_RENDERER_HPP

#include "Theme.hpp"
#include "tools/ToolPresentation.hpp"

#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

ftxui::Element RenderToolPresentationDiffs(const ToolPresentation &presentation,
                                           const Theme &theme, bool expanded);

} // namespace firmius::tui

#endif
