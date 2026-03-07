#include "components/StatusBar.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

ftxui::Component StatusBar(const std::shared_ptr<StatusBarModel> &model) {
  return ftxui::Renderer([model] {
    if (!model) {
      return ftxui::text("");
    }

    // -- Left section: mode badge --
    std::string mode = model->status_text;
    ftxui::Color badge_bg = ftxui::Color::RGB(80, 80, 120);
    ftxui::Color badge_fg = ftxui::Color::RGB(220, 220, 255);

    if (mode == "streaming" || mode == "executing_tool") {
      badge_bg = ftxui::Color::RGB(40, 140, 80);
      badge_fg = ftxui::Color::RGB(220, 255, 220);
    } else if (mode == "idle" || mode == "awaiting_input") {
      badge_bg = ftxui::Color::RGB(60, 60, 100);
      badge_fg = ftxui::Color::RGB(180, 180, 220);
    } else if (mode == "error" || mode == "cancelled") {
      badge_bg = ftxui::Color::RGB(160, 50, 50);
      badge_fg = ftxui::Color::RGB(255, 200, 200);
    } else if (mode == "provider_waiting" || mode == "compacting") {
      badge_bg = ftxui::Color::RGB(140, 120, 40);
      badge_fg = ftxui::Color::RGB(255, 240, 180);
    }

    // Uppercase the mode for display
    std::string mode_upper;
    for (char c : mode)
      mode_upper += static_cast<char>(toupper(c));

    auto mode_badge = ftxui::text(" " + mode_upper + " ") | ftxui::bold |
                      ftxui::color(badge_fg) | ftxui::bgcolor(badge_bg);

    // -- Center section: model name --
    auto model_section = ftxui::text("");
    if (!model->model_name.empty()) {
      model_section = ftxui::text(" " + model->model_name + " ") |
                      ftxui::color(ftxui::Color::RGB(160, 160, 200));
    }

    // -- Right section: purpose / role --
    auto purpose_section = ftxui::text("");
    if (!model->purpose.empty()) {
      purpose_section = ftxui::text(" " + model->purpose + " ") |
                        ftxui::color(ftxui::Color::RGB(120, 120, 160));
    }
    // -- Brand --
    auto brand = ftxui::text(" firmius ") | ftxui::bold |
                 ftxui::color(ftxui::Color::RGB(140, 120, 200));

    // Compose: [MODE] <filler> model <filler> purpose  firmius
    return ftxui::hbox(
               {mode_badge,
                ftxui::text(" ") | ftxui::color(ftxui::Color::RGB(60, 60, 80)),
                model_section, ftxui::filler(), purpose_section}) |
           ftxui::bgcolor(ftxui::Color::RGB(30, 30, 50));
  });
}

} // namespace firmius::tui
