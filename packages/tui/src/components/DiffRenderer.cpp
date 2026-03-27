#include "components/DiffRenderer.hpp"

#include "components/SyntaxHighlighter.hpp"

#include <algorithm>

namespace firmius::tui {

namespace {

ftxui::Element RenderHighlightedContent(const std::string &content,
                                        const std::string &language,
                                        const Theme &theme) {
  if (!language.empty() &&
      SyntaxHighlighter::instance().hasGrammar(language)) {
    auto highlighted =
        SyntaxHighlighter::instance().highlightRenderLines(content, language);
    if (!highlighted.empty()) {
      return highlighted.front() | ftxui::flex_shrink;
    }
  }
  return ftxui::text(content.empty() ? " " : content) |
         ftxui::color(theme.base.fg) | ftxui::flex_shrink;
}

ftxui::Color PrefixColor(const ToolPresentationDiffLine &line,
                         const Theme &theme) {
  if (line.type == '+') {
    return ftxui::Color::Green;
  }
  if (line.type == '-') {
    return theme.status_bar.error.normal.fg;
  }
  return theme.base.dim;
}

std::string FormatLinePrefix(const ToolPresentationDiffLine &line, size_t width) {
  const int line_number = line.type == '-' ? line.old_line : line.new_line;
  std::string prefix(1, line.type == '\0' ? ' ' : line.type);
  prefix += " ";
  if (line_number > 0) {
    auto number = std::to_string(line_number);
    if (number.size() < width) {
      prefix.append(width - number.size(), ' ');
    }
    prefix += number;
  } else {
    prefix.append(width, ' ');
  }
  prefix += " ";
  return prefix;
}

size_t MaxLineNumberWidth(const ToolPresentation &presentation) {
  size_t width = 1;
  for (const auto &section : presentation.diff_sections) {
    for (const auto &line : section.lines) {
      const int line_number = line.type == '-' ? line.old_line : line.new_line;
      if (line_number > 0) {
        width = std::max(width, std::to_string(line_number).size());
      }
    }
  }
  return width;
}

} // namespace

ftxui::Element RenderToolPresentationDiffs(const ToolPresentation &presentation,
                                           const Theme &theme, bool /*expanded*/) {
  if (presentation.diff_sections.empty()) {
    return ftxui::emptyElement();
  }

  const std::string language =
      presentation.diff_source_name.empty()
          ? std::string{}
          : SyntaxHighlighter::instance().detectLanguage(
                presentation.diff_source_name);
  const size_t line_number_width = MaxLineNumberWidth(presentation);
  const auto full_add_bg = ftxui::Color::RGB(18, 72, 40);

  ftxui::Elements rows;
  for (size_t i = 0; i < presentation.diff_sections.size(); ++i) {
    const auto &section = presentation.diff_sections[i];
    if (i > 0) {
      rows.push_back(ftxui::separatorLight() |
                     ftxui::color(theme.base.separator));
    }
    if (!section.title.empty()) {
      rows.push_back(ftxui::text(section.title) | ftxui::bold |
                     ftxui::color(theme.tool_blocks.specific.file_edit.fg));
    }
    if (!section.meta.empty()) {
      rows.push_back(ftxui::paragraph(section.meta) |
                     ftxui::color(theme.base.dim));
    }
    if (section.error_text.has_value() && !section.error_text->empty()) {
      rows.push_back(ftxui::paragraph(*section.error_text) |
                     ftxui::color(theme.status_bar.error.normal.fg));
    }
    if (section.lines.empty()) {
      rows.push_back(ftxui::text("(no textual changes)") |
                     ftxui::color(theme.base.dim));
      continue;
    }

    for (const auto &line : section.lines) {
      const auto bg =
          line.highlight_background ? full_add_bg : theme.tool_blocks.generic_bg;
      auto content =
          RenderHighlightedContent(line.content, language, theme) | ftxui::xflex;
      rows.push_back(ftxui::hbox({
                         ftxui::text(FormatLinePrefix(line, line_number_width)) |
                             ftxui::color(PrefixColor(line, theme)),
                         std::move(content),
                     }) |
                     ftxui::bgcolor(bg) | ftxui::xflex);
    }
  }

  return ftxui::vbox(std::move(rows)) |
         ftxui::bgcolor(theme.tool_blocks.generic_header_bg) | ftxui::xflex;
}

} // namespace firmius::tui
