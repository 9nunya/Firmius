#include "components/FileEditToolBlock.hpp"
#include "UIState.hpp"
#include "components/FileEditDiff.hpp"
#include "components/SyntaxHighlighter.hpp"
#include "ThemeManager.hpp"
#include "utils/Icons.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>

namespace firmius::tui {

namespace {

struct EditPreview {
  std::string op;
  std::string label;
  std::vector<std::string> newLines;
  std::string error;
  int startLine = 0;
  int endLine = 0;
  int newLineCount = 0;
  bool relocated = false;
  std::vector<std::string> oldLines;
};

std::string joinLines(const std::vector<std::string> &lines) {
  std::string result;
  for (const auto &line : lines) {
    result += line;
    result += '\n';
  }
  return result;
}

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

std::vector<std::string> parseStringArray(const rapidjson::Value& array) {
  std::vector<std::string> result;
  if (array.IsArray()) {
    for (const auto& item : array.GetArray()) {
      if (item.IsString()) {
        result.emplace_back(item.GetString());
      }
    }
  }
  return result;
}

ftxui::Element renderPreviewLines(const std::vector<std::string> &lines,
                                  const std::string &language,
                                  int startLine,
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
        ftxui::text("+ " + std::to_string(startLine + static_cast<int>(shown)) +
                    " ") |
            ftxui::color(ftxui::Color::Green),
        renderHighlightedContent(line, language, fallback) | ftxui::flex_shrink,
    }));
    ++shown;
  }
  if (rendered.empty()) {
    rendered.push_back(ftxui::text("(no new lines)") | ftxui::color(dim));
  }
  return ftxui::vbox(rendered);
}

ftxui::Element renderOldPreviewLines(const std::vector<std::string> &lines,
                                     const std::string &language,
                                     int startLine,
                                     size_t maxLines,
                                     ftxui::Color fallback,
                                     ftxui::Color dim,
                                     ftxui::Color minusColor) {
  ftxui::Elements rendered;
  size_t shown = 0;
  for (const auto &line : lines) {
    if (shown >= maxLines) {
      rendered.push_back(ftxui::text("  …") | ftxui::color(dim));
      break;
    }
    rendered.push_back(ftxui::hbox({
        ftxui::text("- " + std::to_string(startLine + static_cast<int>(shown)) +
                    " ") |
            ftxui::color(minusColor),
        renderHighlightedContent(line, language, fallback) | ftxui::flex_shrink,
    }));
    ++shown;
  }
  if (rendered.empty()) {
    rendered.push_back(ftxui::text("(no old lines)") | ftxui::color(dim));
  }
  return ftxui::vbox(rendered);
}

ftxui::Element renderOperationDiff(const EditPreview &preview,
                                   const std::string &language,
                                   ftxui::Color fallback,
                                   ftxui::Color dim,
                                   ftxui::Color minusColor,
                                   size_t maxLines) {
  const auto hunks =
      BuildDiffHunks(joinLines(preview.oldLines), joinLines(preview.newLines));

  if (hunks.empty()) {
    return ftxui::text("(metadata only)") | ftxui::color(dim);
  }

  ftxui::Elements rendered;
  size_t shown = 0;
  for (const auto &hunk : hunks) {
    for (const auto &line : hunk.lines) {
      if (shown >= maxLines) {
        rendered.push_back(ftxui::text("  …") | ftxui::color(dim));
        return ftxui::vbox(rendered);
      }

      const bool isRemoval = line.type == '-';
      const int lineNumber = isRemoval
                                 ? preview.startLine + std::max(0, line.oldLine - 1)
                                 : preview.startLine + std::max(0, line.newLine - 1);
      const std::string prefix =
          std::string(1, line.type) + " " + std::to_string(lineNumber) + " ";
      rendered.push_back(ftxui::hbox({
          ftxui::text(prefix) |
              ftxui::color(isRemoval ? minusColor : ftxui::Color::Green),
          renderHighlightedContent(line.content, language, fallback) |
              ftxui::flex_shrink,
      }));
      ++shown;
    }
  }

  return ftxui::vbox(rendered);
}

void mergeOperationMetadata(const rapidjson::Document &doc,
                            std::vector<EditPreview> &previews) {
  if (!doc.IsObject() || !doc.HasMember("operations") ||
      !doc["operations"].IsArray()) {
    return;
  }

  size_t index = 0;
  for (const auto &operation : doc["operations"].GetArray()) {
    if (!operation.IsObject()) {
      continue;
    }
    if (index >= previews.size()) {
      break;
    }
    auto &preview = previews[index++];
    if (operation.HasMember("start_line") && operation["start_line"].IsInt()) {
      preview.startLine = operation["start_line"].GetInt();
    }
    if (operation.HasMember("end_line") && operation["end_line"].IsInt()) {
      preview.endLine = operation["end_line"].GetInt();
    }
    if (operation.HasMember("new_line_count") &&
        operation["new_line_count"].IsInt()) {
      preview.newLineCount = operation["new_line_count"].GetInt();
    }
    if (operation.HasMember("relocated") && operation["relocated"].IsBool()) {
      preview.relocated = operation["relocated"].GetBool();
    }
    if (preview.label.empty() && operation.HasMember("description") &&
        operation["description"].IsString()) {
      preview.label = operation["description"].GetString();
    }
    if (operation.HasMember("error") && operation["error"].IsString()) {
      preview.error = operation["error"].GetString();
    }
    if (operation.HasMember("old_lines") && operation["old_lines"].IsArray()) {
      preview.oldLines = parseStringArray(operation["old_lines"]);
    }
    if (operation.HasMember("new_lines") && operation["new_lines"].IsArray()) {
      preview.newLines = parseStringArray(operation["new_lines"]);
    }
  }
}

}

ftxui::Component FileEditToolBlock(const std::shared_ptr<ToolCallView> &view) {
  return ftxui::Renderer([view] {
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
    rapidjson::Document result_doc;
    result_doc.Parse(view->result.c_str());
    mergeOperationMetadata(result_doc, previews);

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
            ftxui::Elements op_rows;
            if (!preview.oldLines.empty()) {
              op_rows.push_back(renderOldPreviewLines(preview.oldLines, language, preview.startLine, 4, theme.tool_blocks.specific.file_edit.fg, theme.base.dim, theme.status_bar.error.normal.fg));
            }
            op_rows.push_back(renderPreviewLines(preview.newLines, language, preview.startLine > 0 ? preview.startLine : 1, 4, theme.tool_blocks.specific.file_edit.fg, theme.base.dim));
            diff_elements.push_back(ftxui::vbox({
                ftxui::text(preview.op + " " + preview.label) | ftxui::bold |
                    ftxui::color(theme.tool_blocks.specific.file_edit.fg),
                ftxui::vbox(std::move(op_rows)),
            }));
            ++shown;
          }
        }

        return ftxui::vbox(
                   {header,
                    ftxui::separatorLight() |
                        ftxui::color(theme.base.separator),
                    ftxui::vbox(diff_elements) | ftxui::frame |
                        ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 12),
                    ftxui::hbox({ftxui::text(std::to_string(previews.size()) +
                                             " ops") |
                                     ftxui::color(theme.base.dim),
                                 ftxui::filler(),
                                 ftxui::text(filename) |
                                     ftxui::color(theme.base.dim)})}) |
               ftxui::bgcolor(theme.tool_blocks.generic_bg);
      }

      return header;
    }

    // ── Finished + error ──
    if (!view->success) {
      std::string err_msg = view->result.empty() ? "unknown error" : view->result;
      if (!result_doc.HasParseError() && result_doc.IsObject()) {
        if (result_doc.HasMember("path") && result_doc["path"].IsString() &&
            path_arg.empty()) {
          path_arg = result_doc["path"].GetString();
          filename = path_arg;
          auto slash = path_arg.find_last_of('/');
          if (slash != std::string::npos && slash + 1 < path_arg.size()) {
            filename = path_arg.substr(slash + 1);
          }
        }
        if (result_doc.HasMember("error") && result_doc["error"].IsString()) {
          err_msg = result_doc["error"].GetString();
        }
      }

      using namespace firmius::shared;
      ftxui::Elements rows;
      rows.push_back(ftxui::hbox({
          ftxui::text("▎ ") |
              ftxui::color(theme.status_bar.error.normal.fg),
          ftxui::text(ICON_FILE_EDIT + std::string(" ")) |
              ftxui::color(theme.status_bar.error.normal.fg),
          ftxui::text((filename.empty() ? std::string("file edit") : filename) +
                      " failed") |
              ftxui::bold | ftxui::color(theme.status_bar.error.normal.fg),
      }));
      if (!path_arg.empty()) {
        rows.push_back(ftxui::text(path_arg) | ftxui::color(theme.base.dim));
      }
      rows.push_back(ftxui::paragraph(err_msg) |
                     ftxui::color(theme.status_bar.error.normal.fg));

      std::string language = SyntaxHighlighter::instance().detectLanguage(filename);
      if (!previews.empty()) {
        ftxui::Elements error_rows;
        for (const auto &preview : previews) {
          std::string title =
              preview.label.empty() ? preview.op : preview.op + " " + preview.label;
          ftxui::Elements op_rows;
          op_rows.push_back(ftxui::text(title) | ftxui::bold |
                            ftxui::color(theme.tool_blocks.specific.file_edit.fg));

          if (!preview.error.empty()) {
            op_rows.push_back(ftxui::paragraph(preview.error) |
                              ftxui::color(theme.status_bar.error.normal.fg));
          } else {
            op_rows.push_back(ftxui::text("No error for this operation") |
                              ftxui::color(theme.base.dim));
          }

          if (!preview.newLines.empty()) {
            op_rows.push_back(renderPreviewLines(
                preview.newLines, language,
                preview.startLine > 0 ? preview.startLine : 1, 4,
                theme.tool_blocks.specific.file_edit.fg, theme.base.dim));
          }

          error_rows.push_back(ftxui::vbox(std::move(op_rows)));
        }
        rows.push_back(ftxui::separatorLight() |
                       ftxui::color(theme.base.separator));
        rows.push_back(ftxui::vbox(std::move(error_rows)));
      }

      return ftxui::vbox(std::move(rows)) |
             ftxui::bgcolor(theme.tool_blocks.generic_bg);
    }

    view->show_result = UIState::instance().diffsExpanded;
    int added = 0;
    int removed = 0;
    std::string language =
        SyntaxHighlighter::instance().detectLanguage(filename);
    for (const auto &preview : previews) {
      added += static_cast<int>(preview.newLines.size());
      removed += static_cast<int>(preview.oldLines.size());
    }

    ftxui::Elements rows;

    using namespace firmius::shared;
    rows.push_back(ftxui::hbox({
        ftxui::text("▎ ") |
            ftxui::color(theme.tool_blocks.specific.file_edit.fg),
        ftxui::text(ICON_FILE_EDIT + std::string(" ")) |
            ftxui::color(theme.tool_blocks.generic_icon),
        ftxui::text(filename + " ") | ftxui::bold |
            ftxui::color(theme.tool_blocks.generic_title),
        ftxui::filler(),
        removed > 0 ? (ftxui::text("−" + std::to_string(removed)) |
                       ftxui::color(theme.status_bar.error.normal.bg))
                    : ftxui::text(""),
        added > 0 ? (ftxui::text(" +" + std::to_string(added)) |
                     ftxui::color(theme.syntax.string))
                  : ftxui::text(""),
    }));

    if (is_overwrite || !previews.empty()) {
      ftxui::Elements diff_elements;
      for (size_t i = 0; i < previews.size(); ++i) {
        const auto &preview = previews[i];
        std::string range_label = "line " + std::to_string(preview.startLine);
        if (preview.endLine > 0 && preview.endLine != preview.startLine) {
          range_label += "-" + std::to_string(preview.endLine);
        }
        std::string meta_label = range_label;
        if (preview.newLineCount > 0) {
          meta_label += "  +" + std::to_string(preview.newLineCount);
        }
        if (preview.relocated) {
          meta_label += "  relocated";
        }

        ftxui::Elements operation_rows;
        operation_rows.push_back(renderOperationDiff(
            preview, language, theme.tool_blocks.specific.file_edit.fg,
            theme.base.dim, theme.status_bar.error.normal.fg, SIZE_MAX));

        std::string title =
            preview.label.empty() ? preview.op : preview.op + " " + preview.label;
        diff_elements.push_back(ftxui::vbox({
            ftxui::text(title) | ftxui::bold |
                ftxui::color(theme.tool_blocks.specific.file_edit.fg),
            ftxui::text(meta_label) | ftxui::color(theme.base.dim),
            ftxui::vbox(std::move(operation_rows)),
        }));
      }

      rows.push_back(ftxui::separatorLight() |
                     ftxui::color(theme.base.separator));
      rows.push_back(ftxui::vbox(diff_elements) | ftxui::frame);
    }

    return ftxui::vbox(rows) |
           ftxui::bgcolor(theme.tool_blocks.generic_bg);
  });
}

} // namespace firmius::tui
