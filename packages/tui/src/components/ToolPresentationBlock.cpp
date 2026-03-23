#include "components/ToolPresentationBlock.hpp"

#include "ThemeManager.hpp"
#include "utils/Icons.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>

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

std::string LifecycleBadge(const ToolPresentation &presentation) {
  switch (presentation.lifecycle) {
  case ToolPresentationLifecycle::Preparing:
    return "preparing";
  case ToolPresentationLifecycle::Running:
    return "running";
  case ToolPresentationLifecycle::Success:
    return "done";
  case ToolPresentationLifecycle::Error:
    return "error";
  }
  return "";
}

int DefaultVisibleBodyLines(ToolPresentationLayoutKind layout) {
  switch (layout) {
  case ToolPresentationLayoutKind::BodyFirstStream:
    return 6;
  case ToolPresentationLayoutKind::BodyFirstPreview:
    return 8;
  case ToolPresentationLayoutKind::ResultsList:
    return 10;
  case ToolPresentationLayoutKind::CompactFactCard:
    return 4;
  }
  return 8;
}

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
      body_rows.push_back(ftxui::paragraph(command_line) | ftxui::color(theme.base.fg));
    }
    const int max_output_lines = std::max(1, visible_lines - 1);
    const int total_output_lines =
        static_cast<int>(presentation.body_lines.size() - output_start_index);
    const int shown_output_lines =
        expanded ? total_output_lines : std::min(max_output_lines, total_output_lines);
    const int hidden_output_lines = total_output_lines - shown_output_lines;
    const int first_output_index =
        std::max<int>(0, total_output_lines - shown_output_lines);

    if (hidden_output_lines > 0) {
      body_rows.push_back(
          ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme.base.fg),
            ftxui::text("... +" + std::to_string(hidden_output_lines) + " more lines") |
            ftxui::color(theme.base.dim)
          }));
    }
    for (int i = 0; i < shown_output_lines; ++i) {
      const auto &line =
          presentation.body_lines[output_start_index + first_output_index + i];
      body_rows.push_back(ftxui::paragraph("│ " + line) | ftxui::color(theme.base.fg));
    }
    if (presentation.status_footer.has_value() &&
        !presentation.status_footer->empty()) {
      body_rows.push_back(ftxui::hbox({
                              ftxui::text("╰ ") | ftxui::color(theme.base.fg),
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
    const int max_lines = expanded ? static_cast<int>(presentation.body_lines.size())
                                   : std::min<int>(visible_lines, presentation.body_lines.size());
    for (int i = 0; i < max_lines; ++i) {
      body_rows.push_back(ftxui::paragraph("│ " + presentation.body_lines[static_cast<size_t>(i)]) |
                          ftxui::color(theme.base.fg));
    }
    if (!expanded && static_cast<int>(presentation.body_lines.size()) > max_lines) {
      body_rows.push_back(
          ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme.base.fg),
            ftxui::text("... +" + std::to_string(static_cast<int>(presentation.body_lines.size()) - max_lines) + " more lines") |
            ftxui::color(theme.base.dim)
          }));
    }
  }
  if (body_rows.empty()) {
    return ftxui::emptyElement();
  }
  return ftxui::vbox(std::move(body_rows)) |
         ftxui::bgcolor(theme.tool_blocks.generic_header_bg);
}

ftxui::Element BuildFactsFooter(const ToolPresentation &presentation, const Theme &theme) {
  std::vector<std::string> parts;
  parts.reserve(presentation.footer_badges.size() + 1);
  for (const auto &badge : presentation.footer_badges) {
    if (!badge.empty()) {
      parts.push_back(badge);
    }
  }
  const std::string lifecycle = LifecycleBadge(presentation);
  if (!lifecycle.empty()) {
    parts.push_back(lifecycle);
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
  parts.reserve(presentation.footer_badges.size() + 1);
  for (const auto &badge : presentation.footer_badges) {
    if (!badge.empty()) {
      parts.push_back(badge);
    }
  }
  const std::string lifecycle = LifecycleBadge(presentation);
  if (!lifecycle.empty()) {
    parts.push_back(lifecycle);
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

} // namespace

class ToolPresentationBlockComponent : public ftxui::ComponentBase {
public:
  ToolPresentationBlockComponent(
      std::shared_ptr<firmius::shared::ToolCallView> view,
      std::function<ToolPresentation()> presentation_getter)
      : view_(std::move(view)), presentation_getter_(std::move(presentation_getter)) {
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

    std::string icon = firmius::shared::ICON_GEAR;
    ftxui::Color icon_color = theme.tool_blocks.generic_icon;
    ftxui::Color title_color = theme.tool_blocks.generic_title;
    bool dim_header = false;
    if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
      icon = firmius::shared::ICON_ERROR;
      icon_color = theme.status_bar.error.normal.fg;
      title_color = theme.status_bar.error.normal.fg;
    } else if (presentation.lifecycle == ToolPresentationLifecycle::Success) {
      icon = firmius::shared::ICON_CHECK;
    } else {
      dim_header = true;
    }

    const int body_visible_lines = DefaultVisibleBodyLines(presentation.layout);
    const bool body_has_hidden_lines =
        static_cast<int>(presentation.body_lines.size()) > body_visible_lines;
    const bool has_expand_details =
        presentation.expandable ||
        body_has_hidden_lines ||
        (presentation.density == ToolPresentationDensity::DetailHeavy &&
         !presentation.sections.empty());
    const bool one_line_summary =
        presentation.density == ToolPresentationDensity::OneLineSummary;
    const bool show_expand_toggle =
        !one_line_summary && presentation.expandable && has_expand_details && view_;
    const bool expanded = show_expand_toggle ? presentation.expanded : true;
    if (show_expand_toggle) {
      view_->toggle_label = expanded ? "hide" : "show more";
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
      header.push_back(ftxui::text(" " + icon + " ") | ftxui::color(icon_color));
      if (!presentation.title.empty()) {
        header.push_back(
            ftxui::text(presentation.title) | ftxui::bold | ftxui::color(title_color));
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
      auto body_window =
          BuildBodyWindow(presentation, theme, expanded, body_visible_lines,
                          toggle_button_, show_expand_toggle);
      if (body_window.get() != nullptr) {
        root_rows.push_back(body_window);
      }
    }

    if (!one_line_summary) {
      for (const auto &notice : presentation.notices) {
        root_rows.push_back(ftxui::paragraph(NoticePrefix(notice.kind) + notice.text) |
                            ftxui::color(NoticeColor(theme, notice.kind)));
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
                              ftxui::color(theme.base.dim));
        }
        for (const auto &line : section.lines) {
          root_rows.push_back(ftxui::paragraph("• " + line) | ftxui::color(theme.base.dim));
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

    return ftxui::vbox(root_rows) | ftxui::bgcolor(theme.tool_blocks.generic_bg);
  }

private:
  std::shared_ptr<firmius::shared::ToolCallView> view_;
  std::function<ToolPresentation()> presentation_getter_;
  ftxui::Component toggle_button_;
};

ftxui::Component ToolPresentationBlock(
    const std::shared_ptr<firmius::shared::ToolCallView> &view,
    std::function<ToolPresentation()> presentation_getter) {
  return ftxui::Make<ToolPresentationBlockComponent>(view,
                                                     std::move(presentation_getter));
}

} // namespace firmius::tui
