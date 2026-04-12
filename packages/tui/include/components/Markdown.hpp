#ifndef FIRMIUS_COMPONENTS_MARKDOWN_HPP
#define FIRMIUS_COMPONENTS_MARKDOWN_HPP

#include <ftxui/dom/elements.hpp>
#include <string>

namespace firmius::tui {

// Set the maximum width for markdown rendering (call before RenderMarkdown)
void SetMarkdownWidth(int width);

std::string CollapseExpandedReferencesForDisplay(const std::string &text);
std::string ClampTranscriptTextForDisplay(const std::string &text);

ftxui::Element RenderMarkdown(const std::string& text, bool dim = false);

}

#endif
