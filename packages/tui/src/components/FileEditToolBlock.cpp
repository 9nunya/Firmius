#include "components/FileEditToolBlock.hpp"
#include "components/SyntaxHighlighter.hpp"
#include "ThemeManager.hpp"
#include "UIState.hpp"
#include "utils/Icons.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>

namespace firmius::tui {

namespace {

struct EditPreview {
  std::string op;
  std::string label;
  std::vector<std::string> newLines;
};

ftxui::Element renderHighlightedContent(const std::string &content,
                                        const std::string &language,
                                        ftxui::Color fallback) {
  if (!language.empty() &&
      SyntaxHighlighter::instance().hasGrammar(language)) {
    auto highlighted =
        SyntaxHighlighter::instance().highlightRenderLines(content, language);
    if (!highlighted.empty()) {
      return highlighted.front();
    }
  }
  return ftxui::text(content) | ftxui::color(fallback);
} // namespace

std::vector<EditPreview> parseEditPreviews(const rapidjson::Document &doc) {
  std::vector<EditPreview> previews;
  if (!doc.HasMember("edits") || !doc["edits"].IsArray()) {
    return previews;
  }

  for (const auto &edit : doc["edits"].GetArray()) {
    if (!edit.IsObject()) {
      continue;
    }

    EditPreview preview;
    if (edit.HasMember("op") && edit["op"].IsString()) {
      preview.op = edit["op"].GetString();
    }

    if ((preview.op == "replace_range" || preview.op == "delete_range") &&
        edit.HasMember("start_anchor") && edit["start_anchor"].IsString() &&
        edit.HasMember("end_anchor") && edit["end_anchor"].IsString()) {
      preview.label = std::string(edit["start_anchor"].GetString()) + " -> " +
                      edit["end_anchor"].GetString();
    } else if ((preview.op == "insert_after" || preview.op == "insert_before") &&
               edit.HasMember("anchor") && edit["anchor"].IsString()) {
      preview.label = edit["anchor"].GetString();
    }

    if (edit.HasMember("new_lines") && edit["new_lines"].IsArray()) {
      for (const auto &line : edit["new_lines"].GetArray()) {
        if (line.IsString()) {
          preview.newLines.emplace_back(line.GetString());
        }
      }
    }

    previews.push_back(std::move(preview));
  }

  return previews;
}

ftxui::Element renderPreviewLines(const std::vector<std::string> &lines,
                                  const std::string &language,
                                  size_t maxLines,
                                  ftxui::Color fallback,
                                  ftxui::Color dim) {
  ftxui::Elements rendered;
  size_t shown = 0;
  for (const auto &line : lines) {
    if (shown >= maxLines) {
      rendered.push_back(ftxui::text("  …") | ftxui::color(dim));
      break;
    }
    rendered.push_back(ftxui::hbox({
        ftxui::text("+ ") | ftxui::color(ftxui::Color::Green),
        renderHighlightedContent(line, language, fallback) | ftxui::flex_shrink,
    }));
    ++shown;
  }
  if (rendered.empty()) {
    rendered.push_back(ftxui::text("(no new lines)") | ftxui::color(dim));
  }
  return ftxui::vbox(rendered);
}

}

ftxui::Component FileEditToolBlock(const std::shared_ptr<ToolCallView> &view) {
  auto opt = ftxui::ButtonOption::Simple();
  opt.transform = [](const ftxui::EntryState &s) {
    auto e = ftxui::text(s.label) | ftxui::dim;
    if (s.focused)
      e = e | ftxui::underlined;
    return e;
  };
  if (view) {
    opt.label = &view->toggle_label;
  } else {
    opt.label = "show";
  }
  opt.on_click = [view] {
    if (!view)
      return;
    view->show_result = !view->show_result;
  };

  auto toggle = ftxui::Button(opt);
  auto container = ftxui::Container::Horizontal({toggle});

  return ftxui::Renderer(container, [view, toggle] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    if (!view)
      return ftxui::text("File edit call") | ftxui::color(theme.base.dim);

    // Parse args
    std::string path_arg;
    std::vector<EditPreview> previews;
    bool is_overwrite = false;
    if (!view->args.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("path") && doc["path"].IsString())
          path_arg = doc["path"].GetString();
        if (doc.HasMember("content") && doc["content"].IsString()) {
          is_overwrite = true;
        }
        previews = parseEditPreviews(doc);
      }
    }

    // Get filename from path
    std::string filename = path_arg;
    auto pos = path_arg.find_last_of('/');
    if (pos != std::string::npos && pos + 1 < path_arg.size())
      filename = path_arg.substr(pos + 1);

    if (filename.size() > 50) {
      filename = "…" + filename.substr(filename.size() - 48);
    }

    // ── Preparing / Called ──
    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      std::string action = is_overwrite ? "Writing" : "Editing";

      using namespace firmius::shared;
      auto header = ftxui::hbox(
          {ftxui::text(" " + ICON_GEAR + " ") |
               ftxui::color(theme.tool_blocks.generic_icon),
           ftxui::text(action + " ") | ftxui::bold |
               ftxui::color(theme.tool_blocks.generic_title),
           ftxui::text(filename) |
               ftxui::color(theme.tool_blocks.specific.file_edit.fg)});

      if (view->phase == ToolPhase::Called &&
          (is_overwrite || !previews.empty())) {
        ftxui::Elements diff_elements;
        std::string language =
            SyntaxHighlighter::instance().detectLanguage(filename);

        if (!previews.empty()) {
          size_t shown = 0;
          for (const auto &preview : previews) {
            if (shown >= 3) {
              diff_elements.push_back(ftxui::text("  …") |
                                      ftxui::color(theme.base.dim));
              break;
            }
            diff_elements.push_back(ftxui::vbox({
                ftxui::text(preview.op + " " + preview.label) | ftxui::bold |
                    ftxui::color(theme.tool_blocks.specific.file_edit.fg),
                renderPreviewLines(preview.newLines, language, 4,
                                   theme.tool_blocks.specific.file_edit.fg,
                                   theme.base.dim),
            }));
            ++shown;
          }
        }

        return ftxui::vbox(
                   {header,
                    ftxui::separatorLight() |
                        ftxui::color(theme.tool_blocks.generic_border),
                    ftxui::vbox(diff_elements) | ftxui::frame |
                        ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 12),
                    ftxui::hbox({ftxui::text(std::to_string(previews.size()) +
                                             " ops") |
                                     ftxui::color(theme.base.dim),
                                 ftxui::filler(),
                                 ftxui::text(filename) |
                                     ftxui::color(theme.base.dim)})}) |
               ftxui::borderRounded |
               ftxui::color(theme.tool_blocks.generic_border) |
               ftxui::bgcolor(theme.tool_blocks.generic_bg);
      }

      return header;
    }

    // ── Finished + error ──
    if (!view->success) {
      std::string err_msg = view->result;
      if (err_msg.empty())
        err_msg = "unknown error";
      if (err_msg.size() > 60)
        err_msg = err_msg.substr(0, 57) + "…";

      using namespace firmius::shared;
      return ftxui::hbox({ftxui::text(" " + ICON_ERROR + " ") |
                              ftxui::color(theme.status_bar.error.normal.bg),
                          ftxui::text("Edit failed: " + err_msg) |
                              ftxui::color(theme.status_bar.error.normal.bg)}) |
             ftxui::borderRounded |
             ftxui::color(theme.status_bar.error.normal.bg) |
             ftxui::bgcolor(theme.tool_blocks.generic_bg);
    }

    // ── Finished + success ──
    view->toggle_label = view->show_result ? "hide" : "show diff";

    int added = 0;
    int removed = 0;
    std::string language =
        SyntaxHighlighter::instance().detectLanguage(filename);
    for (const auto &preview : previews) {
      added += static_cast<int>(preview.newLines.size());
      if (preview.op == "replace_range" || preview.op == "delete_range")
        removed += 1;
    }

    ftxui::Elements rows;

    using namespace firmius::shared;
    // Header with stats
    rows.push_back(ftxui::hbox({
        ftxui::text(" " + ICON_FILE_EDIT + " ") |
            ftxui::color(theme.tool_blocks.generic_icon),
        ftxui::text(filename + " ") | ftxui::bold |
            ftxui::color(theme.tool_blocks.generic_title),
        ftxui::text(" [") | ftxui::color(theme.base.dim),
        toggle->Render(),
        ftxui::text("]") | ftxui::color(theme.base.dim),
        ftxui::filler(),
        removed > 0 ? (ftxui::text("−" + std::to_string(removed)) |
                       ftxui::color(theme.status_bar.error.normal.bg))
                    : ftxui::text(""),
        added > 0 ? (ftxui::text(" +" + std::to_string(added)) |
                     ftxui::color(theme.syntax.string))
                  : ftxui::text(""),
    }));

    if (view->show_result && (is_overwrite || !previews.empty())) {
      bool expanded = UIState::instance().diffsExpanded;
      size_t maxOps = expanded ? previews.size() : 3;
      ftxui::Elements diff_elements;
      for (size_t i = 0; i < previews.size() && i < maxOps; ++i) {
        const auto &preview = previews[i];
        diff_elements.push_back(ftxui::vbox({
            ftxui::text(preview.op + " " + preview.label) | ftxui::bold |
                ftxui::color(theme.tool_blocks.specific.file_edit.fg),
            renderPreviewLines(
                preview.newLines, language,
                expanded ? SIZE_MAX
                         : static_cast<size_t>(
                               UIState::instance().maxCollapsedLines),
                theme.tool_blocks.specific.file_edit.fg, theme.base.dim),
        }));
      }
      if (!expanded && previews.size() > maxOps) {
        diff_elements.insert(
            diff_elements.begin(),
            ftxui::text("  … " + std::to_string(previews.size() - maxOps) +
                        " ops hidden (press Ctrl+G to expand)") |
                ftxui::color(theme.base.dim));
      }

      rows.push_back(ftxui::separatorLight() |
                     ftxui::color(theme.tool_blocks.generic_border));
      rows.push_back(ftxui::vbox(diff_elements) | ftxui::frame);
    }

    return ftxui::vbox(rows) | ftxui::borderRounded |
           ftxui::color(theme.tool_blocks.generic_border) |
           ftxui::bgcolor(theme.tool_blocks.generic_bg);
  });
}

} // namespace firmius::tui
