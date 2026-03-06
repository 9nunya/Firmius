#ifndef FIRMIUS_COMPONENTS_MARKDOWN_HPP
#define FIRMIUS_COMPONENTS_MARKDOWN_HPP

#include <ftxui/dom/elements.hpp>
#include <string>

namespace firmius::tui {

ftxui::Element RenderMarkdown(const std::string& text, bool dim = false);

}

#endif
