#include "components/ToolPresentationBlock.hpp"

#include "components/DiffRenderer.hpp"
#include "components/ANSIParser.hpp"
#include "components/GlintEffect.hpp"
#include "SkinConfig.hpp"
#include "UserPreferences.hpp"
#include "UIState.hpp"
#include "ThemeManager.hpp"
#include "utils/Icons.hpp"
#include "components/SyntaxHighlighter.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>


namespace firmius::tui {

namespace {

ftxui::Color NoticeColor(const Theme &theme, ToolPresentationNoticeKind kind) {
  if (kind == ToolPresentationNoticeKind::Warning) {
    return theme.base.highlight;
  }
  if (kind == ToolPresentationNoticeKind::Error) {
    return theme.status_bar.error.normal.fg;
  }
  return theme.base.dim;
}

std::string NoticePrefix(ToolPresentationNoticeKind kind) {
  if (kind == ToolPresentationNoticeKind::Warning) {
    return "warning: ";
  }
  if (kind == ToolPresentationNoticeKind::Error) {
    return "error: ";
  }
  return "note: ";
}

int DefaultVisibleBodyLines(ToolPresentationLayoutKind layout) {
  switch (layout) {
  case ToolPresentationLayoutKind::InlineStatusRow:
    return 1;
  case ToolPresentationLayoutKind::BodyFirstStream:
    return 5;
  case ToolPresentationLayoutKind::BodyFirstPreview:
    return 8;
  case ToolPresentationLayoutKind::DiffPreview:
    return 12;
  case ToolPresentationLayoutKind::ResultsList:
    return 10;
  case ToolPresentationLayoutKind::CompactFactCard:
    return 4;
  }
  return 8;
}

// ANSI Process Rendering Contract:
// 1. ToolPresentation::body_lines contains raw ANSI strings.
// 2. Rendering layer parses ANSI at render-time using firmius::tui::ParseANSI.
ftxui::Element BuildBodyWindow(const ToolPresentation &presentation, const Theme &theme,
                               bool expanded, int visible_lines,
                               const ftxui::Component &toggle_button,
                               bool show_inline_toggle) {
  ftxui::Elements body_rows;
  if (presentation.layout == ToolPresentationLayoutKind::BodyFirstStream) {
    std::string command_line;
    size_t output_start_index = 0;
    if (!presentation.body_lines.empty() &&
        presentation.body_lines.front().rfind("$ ", 0) == 0) {
      command_line = presentation.body_lines.front();
      output_start_index = 1;
      if (presentation.ansi_aware) {
        if (command_line.rfind("$ ", 0) != 0) {
          body_rows.push_back(ParseANSI(command_line));
        } else {
          body_rows.push_back(ftxui::paragraph(command_line) | ftxui::color(theme.base.fg));
        }
      } else {
        body_rows.push_back(ftxui::paragraph(command_line) | ftxui::color(theme.base.fg));
      }
    }
    if (!presentation.custom_body_elements.empty()) {
      for (const auto& element : presentation.custom_body_elements) {
        body_rows.push_back(ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme.base.highlight),
            element | ftxui::xflex
        }));
      }
      // Add a separator between code and output if there is output
      if (presentation.body_lines.size() > output_start_index) {
        body_rows.push_back(ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme.base.highlight),
            ftxui::text("--- output ---") | ftxui::color(theme.base.dim) | ftxui::xflex
        }));
      }
    }

    const int max_output_lines = std::max(1, visible_lines - 1);
    const int total_output_lines =
        static_cast<int>(presentation.body_lines.size() - output_start_index);
    const int shown_output_lines =
        expanded ? total_output_lines : std::min(max_output_lines, total_output_lines);
    const int first_output_index =
        std::max<int>(0, total_output_lines - shown_output_lines);
    for (int i = 0; i < shown_output_lines; ++i) {
      const auto &line =
          presentation.body_lines[output_start_index + first_output_index + i];
      if (presentation.ansi_aware) {
        body_rows.push_back(ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme.base.highlight),
            ParseANSI(line) | ftxui::xflex
        }));
      } else {
        body_rows.push_back(ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme.base.highlight),
            ftxui::paragraph(line) | ftxui::color(theme.base.fg)
        }));
      }
    }
    if (presentation.status_footer.has_value() &&
        !presentation.status_footer->empty()) {
      body_rows.push_back(ftxui::hbox({
                              ftxui::text("╰ ") | ftxui::color(theme.base.highlight),
                              ftxui::paragraph(*presentation.status_footer) |
                                  ftxui::color(theme.base.dim) |
                                  ftxui::flex_shrink,
                          }) |
                          ftxui::xflex);
    }
    if (show_inline_toggle && toggle_button) {
      body_rows.push_back(
          ftxui::hbox({
              ftxui::text("  ") | ftxui::color(theme.base.fg),
              toggle_button->Render(),
          }) |
          ftxui::xflex);
    }
  } else {
    const size_t total_lines = !presentation.custom_body_elements.empty() ? presentation.custom_body_elements.size() : presentation.body_lines.size();
    for (size_t i = 0; i < total_lines; ++i) {
      if (!presentation.custom_body_elements.empty()) {
        body_rows.push_back(ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme.base.highlight),
            presentation.custom_body_elements[i] | ftxui::xflex
        }));
      } else if (presentation.ansi_aware) {
        body_rows.push_back(ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme.base.highlight),
            ParseANSI(presentation.body_lines[i]) | ftxui::xflex
        }));
      } else {
        body_rows.push_back(ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme.base.highlight),
            ftxui::paragraph(presentation.body_lines[i]) | ftxui::color(theme.base.fg)
        }));
      }
    }
  }
  if (body_rows.empty()) {
    return ftxui::emptyElement();
  }
  return ftxui::vbox(std::move(body_rows));
}

ftxui::Element BuildFactsFooter(const ToolPresentation &presentation, const Theme &theme) {
  std::vector<std::string> parts;
  parts.reserve(presentation.footer_badges.size());
  for (const auto &badge : presentation.footer_badges) {
    if (!badge.empty()) {
      parts.push_back(badge);
    }
  }
  if (!parts.empty()) {
    std::string line;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) {
        line += "  •  ";
      }
      line += parts[i];
    }
    return ftxui::paragraph(line) | ftxui::color(theme.base.dim);
  }
  return ftxui::emptyElement();
}

std::string JoinBadgesInline(const ToolPresentation &presentation) {
  std::vector<std::string> parts;
  parts.reserve(presentation.footer_badges.size());
  for (const auto &badge : presentation.footer_badges) {
    if (!badge.empty()) {
      parts.push_back(badge);
    }
  }
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      out += "  •  ";
    }
    out += parts[i];
  }
  return out;
}
ftxui::Element DecorateTitleWithGlint(const std::string &title,
                                      const ftxui::Color &color,
                                      ToolPresentationLifecycle lifecycle,
                                      const Theme &theme) {
  auto base = ftxui::text(title) | ftxui::bold | ftxui::color(color);
  if (lifecycle == ToolPresentationLifecycle::Running) {
    GlintConfig cfg;
    cfg.gradientColors =
        theme.tool_blocks.glint.empty()
            ? std::vector<ftxui::Color>{ftxui::Color::White,
                                        theme.base.highlight}
            : theme.tool_blocks.glint;
    const auto preferences = loadUserPreferences();
    const SkinKind skin = preferences.skin_kind.value_or(SkinKind::Firmius);
    const SkinConfig skin_config =
        skin == SkinKind::Claudex ? preferences.claudex_skin.value_or(defaultSkinConfig(SkinKind::Claudex)) : preferences.firmius_skin.value_or(defaultSkinConfig(SkinKind::Firmius));
    cfg.durationSeconds = glintDurationSeconds(skin_config.glint_speed);
    cfg.intervalSeconds = glintIntervalSeconds(skin_config.glint_speed);
    cfg.easing = GlintEasing::EaseInOut;
    cfg.includeWhitespace = true;
    return GlintEffect(base, cfg)->Render();
  }
  return base;
}


ftxui::Element BuildInlineStatusRow(const ToolPresentation &presentation,
                                    const Theme &theme, const std::string &icon,
                                    const ftxui::Color &icon_color,
                                    const ftxui::Color &title_color, bool dim_row) {
  (void)icon;
  (void)icon_color;
  auto status_dot = [&]() -> ftxui::Element {
    ftxui::Color color = theme.base.dim;
    if (presentation.lifecycle == ToolPresentationLifecycle::Running) {
      color = theme.base.highlight;
    } else if (presentation.lifecycle == ToolPresentationLifecycle::Success) {
      color = theme.modals.highlight_fg;
    } else if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
      color = theme.status_bar.error.normal.fg;
    }
    return ftxui::text("● ") | ftxui::color(color);
  };

  ftxui::Elements row;
  row.push_back(ftxui::text(" "));
  row.push_back(status_dot());
  if (!presentation.title.empty()) {
    row.push_back(DecorateTitleWithGlint(presentation.title, title_color,
                                         presentation.lifecycle, theme));
  }

  const std::string badges = JoinBadgesInline(presentation);
  if (!badges.empty()) {
    row.push_back(ftxui::text("  " + badges) | ftxui::color(theme.base.dim));
  }
  if (presentation.error_text.has_value() && !presentation.error_text->empty()) {
    row.push_back(ftxui::text("  " + *presentation.error_text) |
                  ftxui::color(theme.status_bar.error.normal.fg));
  }

  auto line = ftxui::hbox(std::move(row));
  if (dim_row) {
    line = line | ftxui::dim;
  }
  return line | ftxui::bgcolor(theme.tool_blocks.generic_bg);
}

ftxui::Element BuildClaudexInlineBlock(const ToolPresentation &presentation,
                                       const Theme &theme,
                                       const std::string &icon,
                                       const ftxui::Color &icon_color,
                                       const ftxui::Color &title_color,
                                       bool dim_row,
                                       const ftxui::Component &toggle_button,
                                       bool show_expand_toggle) {
  ftxui::Elements rows;
  rows.push_back(BuildInlineStatusRow(presentation, theme, icon, icon_color,
                                      title_color, dim_row));

  // Claudex uses a single global Ctrl+G toggle for showing/hiding tool details.
  const bool expanded =
      !presentation.expandable ? true : UIState::instance().diffsExpanded;

  const bool uses_diff_layout =
      presentation.layout == ToolPresentationLayoutKind::DiffPreview;
  const bool show_body = !presentation.body_lines.empty() ||
                         !presentation.custom_body_elements.empty() ||
                         uses_diff_layout;

  ToolPresentation compact_presentation = presentation;
  compact_presentation.expanded = expanded;

  // BodyFirstStream already supports an inline status footer row (`╰ ...`). In
  // Claudex we render the footer as a separate compact `└ ...` row, so prevent
  // double-rendering.
  if (compact_presentation.layout == ToolPresentationLayoutKind::BodyFirstStream) {
    compact_presentation.status_footer.reset();
  }

  // Avoid duplication for process tools: Process presentation includes "$ cmd" as
  // first body line, but the title already renders "Bash <cmd>".
  if (!uses_diff_layout &&
      compact_presentation.layout == ToolPresentationLayoutKind::BodyFirstStream &&
      !compact_presentation.body_lines.empty()) {
    const std::string &first_line = compact_presentation.body_lines.front();
    if (first_line.rfind("$ ", 0) == 0 &&
        compact_presentation.title.rfind("Bash ", 0) == 0) {
      const std::string command = first_line.substr(2);
      const std::string title_command = compact_presentation.title.substr(5);
      if (command == title_command) {
        compact_presentation.body_lines.erase(
            compact_presentation.body_lines.begin());
      }
    }
  }

  if (show_body) {
    if (uses_diff_layout) {
      if (expanded) {
        auto body = RenderToolPresentationDiffs(compact_presentation, theme, true);
        if (body.get() != nullptr) {
          rows.push_back(body);
        }
      } else {
        // Collapsed diff: show a compact summary line, but hide the full diff.
        std::string summary;
        const std::string badges = JoinBadgesInline(presentation);
        if (!badges.empty()) {
          summary = badges;
        }
        if (!summary.empty()) {
          summary += " · ";
        }
        summary += "diff hidden";
        rows.push_back(ftxui::text("    └ " + summary) |
                       ftxui::color(theme.base.dim));
      }
    } else {
      auto body = BuildBodyWindow(
          compact_presentation, theme,
          compact_presentation.expandable ? expanded : true,
          DefaultVisibleBodyLines(compact_presentation.layout), toggle_button,
          false);
      if (body.get() != nullptr) {
        rows.push_back(body);
      }
    }
  }

  for (const auto &notice : presentation.notices) {
    rows.push_back(ftxui::hbox({
        ftxui::text("  " + NoticePrefix(notice.kind)) | ftxui::bold |
            ftxui::color(NoticeColor(theme, notice.kind)),
        ftxui::paragraph(notice.text) | ftxui::color(theme.base.fg)
    }));
  }

  if (presentation.error_text.has_value() && !presentation.error_text->empty()) {
    rows.push_back(ftxui::hbox({
        ftxui::text("  error: ") | ftxui::bold |
            ftxui::color(theme.status_bar.error.normal.fg),
        ftxui::paragraph(*presentation.error_text) |
            ftxui::color(theme.status_bar.error.normal.fg)
    }));
  }

  if (presentation.status_footer.has_value() && !presentation.status_footer->empty()) {
    rows.push_back(ftxui::text("    └ " + *presentation.status_footer) |
                   ftxui::color(theme.base.dim));
  }

  if (show_expand_toggle) {
    rows.push_back(ftxui::hbox({
        ftxui::text("    ▸ ") | ftxui::color(theme.base.dim),
        ftxui::text(expanded ? "press ctrl+g to collapse"
                             : "press ctrl+g to reveal") |
            ftxui::color(theme.base.dim),
    }));
  }

  return ftxui::vbox(std::move(rows));
}

} // namespace

class ToolPresentationBlockComponent : public ftxui::ComponentBase {
public:
  ToolPresentationBlockComponent(
      std::shared_ptr<firmius::shared::ToolCallView> view,
      std::function<ToolPresentation()> presentation_getter,
      std::function<bool()> compact_mode_getter)
      : view_(std::move(view)), presentation_getter_(std::move(presentation_getter)),
        compact_mode_getter_(std::move(compact_mode_getter)) {
    auto toggle_option = ftxui::ButtonOption::Simple();
    toggle_option.transform = [](const ftxui::EntryState &state) {
      auto element = ftxui::text(state.label) | ftxui::dim;
      if (state.focused) {
        element = element | ftxui::underlined;
      }
      return element;
    };
    if (view_) {
      toggle_option.label = &view_->toggle_label;
    } else {
      toggle_option.label = "show";
    }
    toggle_option.on_click = [view = view_] {
      if (!view) {
        return;
      }
      view->show_result = !view->show_result;
    };
    toggle_button_ = ftxui::Button(toggle_option);
    Add(toggle_button_);
  }

  ftxui::Element OnRender() override {
    const ToolPresentation presentation = presentation_getter_();
    const auto &theme = ThemeManager::instance().getCurrentTheme();

    std::string icon = presentation.custom_icon.value_or(firmius::shared::ICON_GEAR);
    ftxui::Color icon_color = theme.tool_blocks.generic_icon;
    ftxui::Color title_color = theme.tool_blocks.generic_title;
    bool dim_header = false;
    if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
      icon = firmius::shared::ICON_ERROR;
      icon_color = theme.status_bar.error.normal.fg;
      title_color = theme.status_bar.error.normal.fg;
    } else if (presentation.lifecycle == ToolPresentationLifecycle::Success) {
      icon = presentation.custom_icon.value_or(firmius::shared::ICON_CHECK);
    } else if (presentation.layout == ToolPresentationLayoutKind::InlineStatusRow) {
      icon = firmius::shared::ICON_WAIT;
    } else if (presentation.custom_icon.has_value()) {
      icon = *presentation.custom_icon;
    } else {
      dim_header = true;
    }
    const bool inline_status_row =
        presentation.layout == ToolPresentationLayoutKind::InlineStatusRow;
    const bool compact_mode = compact_mode_getter_ ? compact_mode_getter_() : false;
    if (inline_status_row) {
      return BuildInlineStatusRow(presentation, theme, icon, icon_color,
                                  title_color, dim_header);
    }
    if (compact_mode) {
      ToolPresentation compact = presentation;
      // Claudex drives expansion/collapse globally via UIState (Ctrl+G).
      if (compact.expandable) {
        compact.expanded = UIState::instance().diffsExpanded;
      }
      return BuildClaudexInlineBlock(
          compact, theme, icon, icon_color, title_color, dim_header,
          toggle_button_,
          compact.expandable &&
              compact.layout != ToolPresentationLayoutKind::InlineStatusRow &&
              view_);
    }

    const int body_visible_lines = DefaultVisibleBodyLines(presentation.layout);
    const bool uses_diff_layout =
        presentation.layout == ToolPresentationLayoutKind::DiffPreview;
    const bool body_has_hidden_lines =
        !uses_diff_layout &&
        static_cast<int>(!presentation.custom_body_elements.empty() ? presentation.custom_body_elements.size() : presentation.body_lines.size()) > body_visible_lines;
    const bool has_expand_details =
        (!uses_diff_layout && presentation.expandable) ||
        body_has_hidden_lines ||
        (presentation.density == ToolPresentationDensity::DetailHeavy &&
         !presentation.sections.empty());
    const bool one_line_summary =
        presentation.density == ToolPresentationDensity::OneLineSummary;
    const bool show_expand_toggle =
        !one_line_summary && !uses_diff_layout && presentation.expandable &&
        has_expand_details && view_;
    const bool expanded = show_expand_toggle ? presentation.expanded : true;
    if (show_expand_toggle) {
      view_->toggle_label = expanded ? presentation.toggle_labels.expanded
                                     : presentation.toggle_labels.collapsed;
    }
    const std::string one_line_badges = JoinBadgesInline(presentation);
    const bool has_one_line_error =
        presentation.error_text.has_value() && !presentation.error_text->empty();

    ftxui::Elements root_rows;
    ftxui::Elements header;
    const bool include_subtitle =
        presentation.density == ToolPresentationDensity::DetailHeavy &&
        !presentation.subtitle.empty();
    const bool has_header_content =
        !presentation.title.empty() || include_subtitle ||
        (one_line_summary && (!one_line_badges.empty() || has_one_line_error));
    if (has_header_content) {
      if (!presentation.title.empty()) {
        header.push_back(ftxui::text(" " + icon + " ") | ftxui::color(icon_color));
        header.push_back(
            DecorateTitleWithGlint(presentation.title, title_color,
                                   presentation.lifecycle, theme));
      }
      if (include_subtitle) {
        header.push_back(ftxui::text(" " + presentation.subtitle) |
                         ftxui::color(theme.base.dim));
      }
    }

    if (!header.empty()) {
      auto header_row = ftxui::hbox(header);
      if (dim_header) {
        header_row = header_row | ftxui::dim;
      }
      root_rows.push_back(header_row);
    }
    if (one_line_summary && !one_line_badges.empty()) {
      root_rows.push_back(ftxui::paragraph(one_line_badges) |
                          ftxui::color(theme.base.dim));
    }
    if (one_line_summary && has_one_line_error) {
      root_rows.push_back(ftxui::paragraph(*presentation.error_text) |
                          ftxui::color(theme.status_bar.error.normal.fg));
    }

    if (!one_line_summary && !presentation.compact_summary.empty() &&
        presentation.compact_summary != presentation.title) {
      root_rows.push_back(ftxui::paragraph(presentation.compact_summary) |
                          ftxui::color(theme.base.dim));
    }

    if (!one_line_summary &&
        presentation.layout != ToolPresentationLayoutKind::CompactFactCard) {
      auto body_window = uses_diff_layout
                             ? RenderToolPresentationDiffs(presentation, theme, true)
                             : BuildBodyWindow(presentation, theme, expanded,
                                               body_visible_lines, toggle_button_,
                                               show_expand_toggle);
      if (body_window.get() != nullptr) {
        root_rows.push_back(body_window);
      }
    }

    if (!one_line_summary) {
      for (const auto &notice : presentation.notices) {
        root_rows.push_back(ftxui::hbox({
            ftxui::text(NoticePrefix(notice.kind)) | ftxui::bold | ftxui::color(NoticeColor(theme, notice.kind)),
            ftxui::paragraph(notice.text) | ftxui::color(theme.base.fg)
        }));
      }
    }

    if (!one_line_summary && presentation.error_text.has_value()) {
      root_rows.push_back(ftxui::paragraph(presentation.error_text.value()) |
                          ftxui::color(theme.status_bar.error.normal.fg));
    }

    if (presentation.layout == ToolPresentationLayoutKind::CompactFactCard) {
      for (const auto &fact : presentation.facts) {
        root_rows.push_back(ftxui::paragraph(fact.key + ": " + fact.value) |
                            ftxui::color(theme.base.dim));
      }
    }

    const bool show_detail_sections =
        expanded && presentation.density == ToolPresentationDensity::DetailHeavy &&
        presentation.layout != ToolPresentationLayoutKind::CompactFactCard &&
        (!presentation.sections.empty() || !presentation.facts.empty());
    if (show_detail_sections) {
      for (const auto &section : presentation.sections) {
        if (!section.title.empty()) {
          root_rows.push_back(ftxui::text(section.title) | ftxui::bold |
                              ftxui::color(NoticeColor(theme, section.kind)));
        }
        for (const auto &line : section.lines) {
          root_rows.push_back(ftxui::hbox({
              ftxui::text("• ") | ftxui::color(NoticeColor(theme, section.kind)),
              ftxui::paragraph(line) | ftxui::color(theme.base.fg)
          }));
        }
      }
      if (!presentation.facts.empty()) {
        for (const auto &fact : presentation.facts) {
          if (!fact.key.empty() && !fact.value.empty()) {
            root_rows.push_back(ftxui::paragraph(fact.key + ": " + fact.value) |
                                ftxui::color(theme.base.dim));
          }
        }
      }
    }

    auto footer = one_line_summary ? ftxui::emptyElement() : BuildFactsFooter(presentation, theme);
    const bool detached_footer_in_window =
        presentation.layout == ToolPresentationLayoutKind::BodyFirstStream &&
        ((presentation.status_footer.has_value() &&
          !presentation.status_footer->empty()) ||
         show_expand_toggle);
    if (!detached_footer_in_window && footer.get() != nullptr) {
      root_rows.push_back(footer);
    }

    if (!detached_footer_in_window && presentation.status_footer.has_value() &&
        !presentation.status_footer->empty()) {
      if (show_expand_toggle) {
        root_rows.push_back(
            ftxui::hbox({
              ftxui::paragraph(*presentation.status_footer) | ftxui::color(theme.base.dim),
              ftxui::text("  ") | ftxui::dim,
              toggle_button_->Render(),
            }));
      } else {
        root_rows.push_back(ftxui::paragraph(*presentation.status_footer) |
                            ftxui::color(theme.base.dim));
      }
    } else if (!detached_footer_in_window && show_expand_toggle) {
      root_rows.push_back(toggle_button_->Render());
    }

    return ftxui::vbox(root_rows);
  }

private:
  std::shared_ptr<firmius::shared::ToolCallView> view_;
  std::function<ToolPresentation()> presentation_getter_;
  std::function<bool()> compact_mode_getter_;
  ftxui::Component toggle_button_;
};

ftxui::Component ToolPresentationBlock(
    const std::shared_ptr<firmius::shared::ToolCallView> &view,
    std::function<ToolPresentation()> presentation_getter,
    std::function<bool()> compact_mode_getter) {
  return ftxui::Make<ToolPresentationBlockComponent>(view,
                                                     std::move(presentation_getter),
                                                     std::move(compact_mode_getter));
}

} // namespace firmius::tui
