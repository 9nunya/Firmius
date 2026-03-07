#include "components/FileEditToolBlock.hpp"
#include "components/ToolWindow.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui {

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
    if (!view)
      return ftxui::text("[file_edit] <null>") | ftxui::dim;

    // Parse args — tool uses old_string/new_string (not target_content)
    std::string path_arg;
    std::string old_string;
    std::string new_string;
    bool is_overwrite = false;
    if (!view->args.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("path") && doc["path"].IsString())
          path_arg = doc["path"].GetString();
        if (doc.HasMember("old_string") && doc["old_string"].IsString())
          old_string = doc["old_string"].GetString();
        if (doc.HasMember("new_string") && doc["new_string"].IsString())
          new_string = doc["new_string"].GetString();
        if (doc.HasMember("content") && doc["content"].IsString())
          is_overwrite = true;
      }
    }

    // ── Preparing / Called ──
    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      std::string action = is_overwrite ? "Writing " : "Editing ";
      auto header =
          ftxui::text("[~] " + action + path_arg + "...") | ftxui::dim;

      // Show diff preview during Called if we have content
      if (view->phase == ToolPhase::Called && !old_string.empty()) {
        std::vector<ftxui::Element> diff_lines;

        // Count lines for +/- stats
        int removed = 0, added = 0;

        std::istringstream old_ss(old_string);
        std::string line;
        while (std::getline(old_ss, line)) {
          diff_lines.push_back(ftxui::hbox(
              {ftxui::text("-| ") | ftxui::dim |
                   ftxui::color(ftxui::Color::Red),
               ftxui::text(line) | ftxui::color(ftxui::Color::Red)}));
          removed++;
        }

        std::istringstream new_ss(new_string);
        while (std::getline(new_ss, line)) {
          diff_lines.push_back(ftxui::hbox(
              {ftxui::text("+| ") | ftxui::dim |
                   ftxui::color(ftxui::Color::Green),
               ftxui::text(line) | ftxui::color(ftxui::Color::Green)}));
          added++;
        }

        std::string footer = "editing " + path_arg + " +" +
                             std::to_string(added) + " -" +
                             std::to_string(removed);

        return ftxui::vbox({header, ToolWindow(diff_lines, footer)});
      }

      return header;
    }

    // ── Finished + error ──
    if (!view->success) {
      std::string err_msg = view->result;
      if (err_msg.empty())
        err_msg = "unknown error";
      return ftxui::text("[x] Edit " + path_arg + " failed: " + err_msg) |
             ftxui::color(ftxui::Color::Red);
    }

    // ── Finished + success ──
    view->toggle_label = view->show_result ? "hide" : "show diff";

    // Count lines for stats
    int removed = 0, added = 0;
    {
      std::istringstream ss(old_string);
      std::string line;
      while (std::getline(ss, line))
        removed++;
    }
    {
      std::istringstream ss(new_string);
      std::string line;
      while (std::getline(ss, line))
        added++;
    }

    std::vector<ftxui::Element> rows;
    rows.push_back(ftxui::hbox(
        {ftxui::text("[+] Edited " + path_arg + " +" + std::to_string(added) +
                     " -" + std::to_string(removed)) |
             ftxui::bold,
         ftxui::text(" [") | ftxui::dim, toggle->Render(),
         ftxui::text("]") | ftxui::dim}));

    if (view->show_result && (!old_string.empty() || !new_string.empty())) {
      std::vector<ftxui::Element> diff_lines;

      std::istringstream old_ss(old_string);
      std::string line;
      while (std::getline(old_ss, line)) {
        diff_lines.push_back(ftxui::hbox(
            {ftxui::text("-| ") | ftxui::dim | ftxui::color(ftxui::Color::Red),
             ftxui::text(line) | ftxui::color(ftxui::Color::Red)}));
      }

      std::istringstream new_ss(new_string);
      while (std::getline(new_ss, line)) {
        diff_lines.push_back(ftxui::hbox(
            {ftxui::text("+| ") | ftxui::dim |
                 ftxui::color(ftxui::Color::Green),
             ftxui::text(line) | ftxui::color(ftxui::Color::Green)}));
      }

      std::string footer = "edited " + path_arg;
      rows.push_back(ToolWindow(diff_lines, footer));
    }

    return ftxui::vbox(rows);
  });
}

} // namespace firmius::tui
