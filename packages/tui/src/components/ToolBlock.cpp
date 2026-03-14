#include "components/ToolBlock.hpp"
#include "ThemeManager.hpp"
#include "components/FileEditToolBlock.hpp"
#include "components/FileReadToolBlock.hpp"
#include "components/GlintEffect.hpp"
#include "components/ListDirectoryToolBlock.hpp"
#include "components/ProcessExecuteToolBlock.hpp"
#include "components/SubagentToolBlock.hpp"
#include "components/SubagentWaitToolBlock.hpp"
#include "utils/ErrorCleaner.hpp"
#include "utils/Icons.hpp"
#include "utils/ToolSummaries.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>
#include <vector>

namespace firmius::tui {

using firmius::shared::SummarizeToolCall;
using firmius::shared::TailLines;

ftxui::Component ToolBlock(const std::shared_ptr<ToolCallView> &view,
                           HistoryGetter sub_history_getter,
                           StreamGetter sub_stream_getter) {
  if (view) {
    if (view->name == "list_directory") {
      return ListDirectoryToolBlock(view);
    } else if (view->name == "file_read") {
      return FileReadToolBlock(view);
    } else if (view->name == "file_edit") {
      return FileEditToolBlock(view);
    } else if (view->name == "process_execute" ||
               view->name == "process_spawn") {
      return ProcessExecuteToolBlock(view);
    } else if (view->name == "summon_subagent") {
      return SubagentToolBlock(view, sub_history_getter, sub_stream_getter);
    } else if (view->name == "subagent_wait") {
      return SubagentWaitToolBlock(view);
    }
  }

  // Generic fallback for unhandled tools
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
    if (!view) {
      return ftxui::text("Tool call") | ftxui::dim;
    }

    std::string summary =
        SummarizeToolCall(view->name, view->args, view->phase);

    const auto &theme = ThemeManager::instance().getCurrentTheme();

    // Preparing state with animated glint
    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {

      GlintConfig cfg;
      cfg.target = GlintConfig::Target::Text;
      if (!theme.tool_blocks.glint.empty()) {
        cfg.gradientColors = theme.tool_blocks.glint;
      } else {
        cfg.gradientColors = {theme.tool_blocks.generic_title, theme.base.fg,
                              theme.tool_blocks.generic_title};
      }
      cfg.glintSize = 14;
      cfg.intervalSeconds = 1.5f;
      cfg.durationSeconds = 1.2f;
      cfg.easing = GlintEasing::EaseInOut;

      using namespace firmius::shared;
      auto loading_text = ftxui::text(ICON_GEAR + " " + summary) | ftxui::bold;
      auto loading = GlintEffect(loading_text, cfg);

      return ftxui::hbox({ftxui::text("▸ ") |
                              ftxui::color(theme.tool_blocks.generic_icon),
                          loading->Render() |
                              ftxui::color(theme.tool_blocks.generic_title)}) |
             ftxui::dim;
    }

    // Success state
    if (view->success) {
      view->toggle_label = view->show_result ? "hide" : "show";
      bool can_toggle = !view->result.empty();

      ftxui::Elements rows;

      using namespace firmius::shared;
      
      // Build a better success summary with result info for grep/glob
      std::string success_summary;
      std::string result_info;
      
      if (view->name == "grep" || view->name == "glob") {
        rapidjson::Document doc;
        doc.Parse(view->result.c_str());
        size_t match_count = 0;
        if (!doc.HasParseError() && doc.IsArray()) {
          match_count = doc.Size();
        }
        result_info = " (" + std::to_string(match_count) + " matches)";
        
        if (view->name == "grep") {
          rapidjson::Document argsDoc;
          argsDoc.Parse(view->args.c_str());
          std::string pattern = "";
          if (!argsDoc.HasParseError() && argsDoc.IsObject() && 
              argsDoc.HasMember("pattern") && argsDoc["pattern"].IsString()) {
            pattern = argsDoc["pattern"].GetString();
          }
          success_summary = "grep \"" + firstWords(pattern, 2) + "\"";
        } else {
          rapidjson::Document argsDoc;
          argsDoc.Parse(view->args.c_str());
          std::string pattern = "";
          if (!argsDoc.HasParseError() && argsDoc.IsObject() && 
              argsDoc.HasMember("pattern") && argsDoc["pattern"].IsString()) {
            pattern = argsDoc["pattern"].GetString();
          }
          success_summary = "glob \"" + pattern + "\"";
        }
      } else {
        success_summary = summary;
      }
      
      // Header row with icon and toggle
      ftxui::Elements header;
      header.push_back(ftxui::text(" " + ICON_CHECK + " ") |
                       ftxui::color(theme.tool_blocks.generic_icon));
      header.push_back(ftxui::text(success_summary + result_info + " ") | ftxui::bold |
                       ftxui::color(theme.tool_blocks.generic_title));

      if (can_toggle) {
        header.push_back(ftxui::text("[") | ftxui::dim);
        header.push_back(toggle->Render());
        header.push_back(ftxui::text("]") | ftxui::dim);
      }

      rows.push_back(ftxui::hbox(header));

      // Expandable result with pretty formatting for grep/glob
      if (view->show_result && !view->result.empty()) {
        if (view->name == "grep" || view->name == "glob") {
          // Pretty print grep/glob results
          rapidjson::Document doc;
          doc.Parse(view->result.c_str());
          if (!doc.HasParseError() && doc.IsArray()) {
            ftxui::Elements result_lines;
            for (rapidjson::SizeType i = 0; i < doc.Size() && i < 20; i++) {
              const auto &item = doc[i];
              std::string line;
              if (item.IsString()) {
                line = item.GetString();
              } else if (item.IsObject() && item.HasMember("path") && item["path"].IsString()) {
                line = item["path"].GetString();
                if (item.HasMember("type") && item["type"].IsString()) {
                  std::string type = item["type"].GetString();
                  std::string prefix = (type == "directory") ? ICON_FOLDER : ICON_FILE;
                  line = prefix + " " + line;
                }
              }
              if (line.size() > 80) {
                line = line.substr(0, 77) + "…";
              }
              result_lines.push_back(ftxui::text(line) |
                                     ftxui::color(theme.base.dim));
            }
            if (doc.Size() > 20) {
              result_lines.push_back(ftxui::text("… and " + std::to_string(doc.Size() - 20) + " more") |
                                     ftxui::dim);
            }
            rows.push_back(ftxui::separatorLight());
            rows.push_back(ftxui::vbox(result_lines) | ftxui::frame |
                           ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 15));
          }
        } else {
          // Generic result display
          auto tail = TailLines(view->result, 5);
          ftxui::Elements result_lines;
          for (const auto &line : tail) {
            result_lines.push_back(ftxui::text(line) |
                                   ftxui::color(theme.base.dim));
          }
          rows.push_back(ftxui::separatorLight());
          rows.push_back(ftxui::vbox(result_lines) | ftxui::frame |
                         ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 8));
        }
      }

      return ftxui::vbox(rows);
    }

    // Error state
    view->phase = ToolPhase::Error;
    std::string err = firmius::shared::ErrorCleaner::clean(view->result);
    if (err.size() > 400)
      err = err.substr(0, 397) + "…";

    using namespace firmius::shared;
    // Build a better error summary that includes tool name and key args
    std::string error_summary;
    if (view->name == "grep") {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      std::string pattern = "";
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("pattern") &&
          doc["pattern"].IsString()) {
        pattern = doc["pattern"].GetString();
      }
      error_summary = "grep \"" + firstWords(pattern, 2) + "\"";
    } else if (view->name == "glob") {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      std::string pattern = "";
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("pattern") &&
          doc["pattern"].IsString()) {
        pattern = doc["pattern"].GetString();
      }
      error_summary = "glob \"" + pattern + "\"";
    } else {
      error_summary = SummarizeToolCall(view->name, view->args, ToolPhase::Called);
    }

    return ftxui::vbox({
               ftxui::hbox(
                   {ftxui::text(" " + ICON_ERROR + " ") |
                        ftxui::color(theme.status_bar.error.normal.fg),
                    ftxui::text(error_summary + " failed") | ftxui::bold |
                        ftxui::color(theme.status_bar.error.normal.fg)}),
               ftxui::paragraph("  " + err) |
                   ftxui::color(theme.status_bar.error.normal.fg) |
                   ftxui::flex_shrink,
           }) |
           ftxui::flex_shrink;
  });
}

} // namespace firmius::tui
