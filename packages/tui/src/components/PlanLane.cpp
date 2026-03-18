#include "components/PlanLane.hpp"
#include "ThemeManager.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

namespace firmius::tui {

namespace {

class PlanLaneComponentBase : public ftxui::ComponentBase {
public:
  explicit PlanLaneComponentBase(std::shared_ptr<PlanLaneModel> model)
      : model_(std::move(model)) {}

  ftxui::Element OnRender() override {
    if (!model_ || !model_->visible) {
      return ftxui::text("");
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const bool compact_mode = ftxui::Terminal::Size().dimx < 90;

    ftxui::Elements summary_parts;
    summary_parts.push_back(ftxui::text(model_->expanded ? "▾ " : "▸ ") |
                            ftxui::color(theme.base.dim));
    summary_parts.push_back(ftxui::text(model_->collapsed_summary) |
                            ftxui::bold | ftxui::color(theme.base.fg) |
                            ftxui::flex);

    auto summary_row = ftxui::hbox(std::move(summary_parts)) | ftxui::xflex;

    if (!model_->expanded) {
      return summary_row;
    }

    ftxui::Elements rows;
    rows.push_back(summary_row);

    for (const auto &chunk : model_->chunks) {
      std::string row_title = chunk.title;
      if (compact_mode && row_title.size() > 42) {
        row_title = row_title.substr(0, 39) + "...";
      }

      ftxui::Elements row_parts;
      row_parts.push_back(ftxui::text("  - ") | ftxui::color(theme.base.dim));
      row_parts.push_back(ftxui::text(row_title) |
                          ftxui::color(theme.base.fg) | ftxui::flex);
      row_parts.push_back(ftxui::text(" " + chunk.status_label) |
                          ftxui::color(theme.base.dim));
      rows.push_back(ftxui::hbox(std::move(row_parts)) | ftxui::xflex);
    }

    return ftxui::vbox(std::move(rows));
  }

private:
  std::shared_ptr<PlanLaneModel> model_;
};

} // namespace

ftxui::Component PlanLane(const std::shared_ptr<PlanLaneModel> &model) {
  return std::make_shared<PlanLaneComponentBase>(model);
}

} // namespace firmius::tui
