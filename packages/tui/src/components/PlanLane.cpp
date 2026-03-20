#include "components/PlanLane.hpp"
#include "ThemeManager.hpp"
#include "components/GlintEffect.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <array>

namespace firmius::tui {

namespace {

struct StatusStyle {
  const char *icon;
  ftxui::Color color;
  const char *chip_label;
};

StatusStyle styleFor(const Theme &theme, shared::WorkChunkStatus status) {
  using shared::WorkChunkStatus;
  switch (status) {
  case WorkChunkStatus::Ready:
    return {">", theme.base.highlight, "ready"};
  case WorkChunkStatus::InProgress:
    return {"*", theme.status_bar.streaming.normal.fg, "live"};
  case WorkChunkStatus::Implemented:
    return {"+", theme.status_bar.executing_tool.normal.fg, "implemented"};
  case WorkChunkStatus::Verifying:
    return {"?", theme.status_bar.provider_waiting.normal.fg, "verify"};
  case WorkChunkStatus::Done:
    return {"#", theme.status_bar.idle.normal.fg, "done"};
  case WorkChunkStatus::Blocked:
    return {"!", theme.status_bar.error.normal.fg, "blocked"};
  case WorkChunkStatus::Failed:
    return {"x", theme.status_bar.error.normal.fg, "failed"};
  case WorkChunkStatus::Cancelled:
    return {"-", theme.base.dim, "cancelled"};
  }
  return {"?", theme.base.dim, "unknown"};
}

ftxui::Element chip(const std::string &label, ftxui::Color fg,
                    ftxui::Color bg) {
  return ftxui::text(" " + label + " ") | ftxui::bold | ftxui::color(fg) |
         ftxui::bgcolor(bg);
}

ftxui::Color statusChipBackground(const Theme &theme,
                                  shared::WorkChunkStatus status) {
  switch (status) {
  case shared::WorkChunkStatus::Blocked:
  case shared::WorkChunkStatus::Failed:
    return theme.status_bar.error.normal.fg;
  case shared::WorkChunkStatus::Cancelled:
    return theme.base.dim;
  default:
    return theme.agent_strip.bg;
  }
}

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

    std::array<int, 8> counts = {};
    for (const auto &chunk : model_->chunks) {
      counts[static_cast<size_t>(chunk.status)]++;
    }

    ftxui::Elements summary_parts;
    summary_parts.push_back(ftxui::text(model_->expanded ? "▾ " : "▸ ") |
                            ftxui::color(theme.base.dim));
    summary_parts.push_back(chip("PLAN", theme.base.bg, theme.base.highlight));
    summary_parts.push_back(ftxui::text(" " + model_->plan_title) |
                            ftxui::bold | ftxui::color(theme.base.fg) |
                            ftxui::flex);
    if (counts[static_cast<size_t>(shared::WorkChunkStatus::InProgress)] > 0) {
      summary_parts.push_back(ftxui::text(" "));
      summary_parts.push_back(chip(
          std::to_string(
              counts[static_cast<size_t>(shared::WorkChunkStatus::InProgress)]) +
              " live",
          theme.base.fg, theme.status_bar.streaming.normal.fg));
    }
    if (counts[static_cast<size_t>(shared::WorkChunkStatus::Verifying)] > 0) {
      summary_parts.push_back(ftxui::text(" "));
      summary_parts.push_back(chip(
          std::to_string(
              counts[static_cast<size_t>(shared::WorkChunkStatus::Verifying)]) +
              " verifying",
          theme.base.fg, theme.status_bar.provider_waiting.normal.fg));
    }
    if (counts[static_cast<size_t>(shared::WorkChunkStatus::Ready)] > 0) {
      summary_parts.push_back(ftxui::text(" "));
      summary_parts.push_back(chip(
          std::to_string(
              counts[static_cast<size_t>(shared::WorkChunkStatus::Ready)]) +
              " queued",
          theme.base.fg, theme.base.dim));
    }

    auto summary_row = ftxui::hbox(std::move(summary_parts)) |
                       ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex;

    for (auto status : {shared::WorkChunkStatus::Failed,
                        shared::WorkChunkStatus::Blocked,
                        shared::WorkChunkStatus::Cancelled}) {
      const int count = counts[static_cast<size_t>(status)];
      if (count <= 0) {
        continue;
      }
      auto style = styleFor(theme, status);
      summary_row = ftxui::hbox({
                        summary_row,
                        ftxui::text(" "),
                        chip(std::to_string(count) + " " + style.chip_label,
                             theme.base.fg,
                             statusChipBackground(theme, status)),
                    }) |
                    ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex;
    }

    if (!model_->expanded) {
      return summary_row | ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex;
    }

    ftxui::Elements rows;
    rows.push_back(summary_row);

    for (const auto &chunk : model_->chunks) {
      const auto style = styleFor(theme, chunk.status);
      std::string row_title = chunk.title;
      
      // V2: Show (N tasks) hint for task-bearing chunks
      std::string task_hint;
      if (chunk.task_count.has_value() && chunk.task_count.value() > 0) {
        task_hint = " (" + std::to_string(chunk.task_count.value()) + " tasks)";
      }
      
      if (compact_mode && row_title.size() > 42) {
        row_title = row_title.substr(0, 39) + "...";
      }

      ftxui::Elements row_parts;
      row_parts.push_back(ftxui::text(" " + std::string(style.icon) + " ") |
                          ftxui::bold | ftxui::color(style.color));
      auto title_el =
          ftxui::text(row_title) |
          ftxui::color(chunk.status == shared::WorkChunkStatus::InProgress ||
                               chunk.status == shared::WorkChunkStatus::Verifying
                           ? theme.base.highlight
                           : theme.base.fg);
      if (chunk.status == shared::WorkChunkStatus::InProgress) {
        title_el = title_el | ftxui::bold;
      }
      row_parts.push_back(title_el | ftxui::flex);
      
      // V2: Add task hint if present
      if (!task_hint.empty()) {
        row_parts.push_back(ftxui::text(task_hint) | ftxui::dim |
                            ftxui::color(theme.base.dim));
      }
      
      row_parts.push_back(ftxui::text(chunk.status_label) | ftxui::bold |
                          ftxui::color(style.color));
      auto row = ftxui::hbox(std::move(row_parts)) | ftxui::xflex;
      if (chunk.status == shared::WorkChunkStatus::InProgress) {
        GlintConfig cfg;
        cfg.target = GlintConfig::Target::Text;
        cfg.gradientColors = theme.tool_blocks.glint.empty()
                                 ? std::vector<ftxui::Color>{
                                       style.color, theme.base.fg, style.color}
                                 : theme.tool_blocks.glint;
        cfg.glintSize = 10;
        cfg.intervalSeconds = 1.2f;
        cfg.durationSeconds = 0.9f;
        cfg.easing = GlintEasing::EaseInOut;
        row = GlintEffect(row, cfg)->Render();
      }
      rows.push_back(row);
    }

    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme.agent_strip.bg) |
           ftxui::xflex;
  }

private:
  std::shared_ptr<PlanLaneModel> model_;
};

} // namespace

ftxui::Component PlanLane(const std::shared_ptr<PlanLaneModel> &model) {
  return std::make_shared<PlanLaneComponentBase>(model);
}

} // namespace firmius::tui
