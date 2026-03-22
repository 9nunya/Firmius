#ifndef FIRMIUS_TUI_COMPONENTS_ERROR_DISPLAY_HPP
#define FIRMIUS_TUI_COMPONENTS_ERROR_DISPLAY_HPP

#include "Context.hpp"
#include "Theme.hpp"
#include "Message.hpp"
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

struct ErrorSection {
  std::string label;
  std::string content;
  bool is_metadata = false;
};

struct ParsedErrorDetails {
  std::string headline;
  std::vector<ErrorSection> metadata;
  std::string raw_body_label;
  std::string raw_body_content;
  bool has_json = false;
  std::string pretty_json;
  std::string trailing_details;
};

ParsedErrorDetails ParseErrorDetails(const std::string &details);

ftxui::Element RenderNoticeDisplay(const Theme &theme,
                                   const firmius::shared::NoticeContent &notice);

ftxui::Element RenderErrorDisplay(const Theme &theme,
                                   const firmius::shared::ErrorContent &error);

} // namespace firmius::tui

#endif
