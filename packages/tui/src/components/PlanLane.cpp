#include "components/PlanLane.hpp"
#include "ThemeManager.hpp"
#include "components/ScrollableBox.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <array>

namespace firmius::tui {

namespace {

struct StatusStyle {
  const char *icon;
  ftxui::Color color;
};

StatusStyle styleFor(const Theme &theme, shared::WorkChunkStatus status) {
  using shared::WorkChunkStatus;
  switch (status) {
  case WorkChunkStatus::Ready:
    return {"", theme.base.highlight};
  case WorkChunkStatus::InProgress:
    return {"", theme.status_bar.streaming.normal.fg};
  case WorkChunkStatus::Implemented:
    return {"󰄬", theme.status_bar.executing_tool.normal.fg};
  case WorkChunkStatus::Verifying:
    return {"", theme.status_bar.provider_waiting.normal.fg};
  case WorkChunkStatus::Done:
    return {"", theme.status_bar.idle.normal.fg};
  case WorkChunkStatus::Blocked:
    return {"", theme.status_bar.error.normal.fg};
  case WorkChunkStatus::Failed:
    return {"", theme.status_bar.error.normal.fg};
  case WorkChunkStatus::Cancelled:
    return {"", theme.base.dim};
  }
  return {"", theme.base.dim};
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
      : model_(std::move(model)) {
    body_renderer_ = ftxui::Renderer([this] { return RenderBody(); });
    scrollable_ = ScrollableBox(
        body_renderer_, {.startAtBottom = false, .overlayScrollbar = true});
    Add(scrollable_);
  }

  ftxui::Element OnRender() override {
    if (!model_ || !model_->visible) {
      return ftxui::text("");
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();

    if (model_->executor_task_view) {
      ftxui::Elements header_parts;
      header_parts.push_back(chip("󰐃", theme.base.bg, theme.base.highlight));
      header_parts.push_back(ftxui::text(" "));
      header_parts.push_back(ftxui::paragraph(model_->executor_chunk_title) |
                             ftxui::bold | ftxui::color(theme.base.fg));
      if (!model_->executor_tasks.empty()) {
        header_parts.push_back(ftxui::text(" "));
        header_parts.push_back(
            chip(" " + std::to_string(model_->executor_tasks.size()),
                 theme.base.fg, theme.agent_strip.bg));
      }
      auto summary_row = ftxui::hflow(std::move(header_parts)) |
                         ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex;
      auto body = scrollable_
                      ? scrollable_->Render() | ftxui::xflex | ftxui::yflex
                              : ftxui::text("");
      return ftxui::vbox({summary_row, body}) |
             ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex;
    }

    std::array<int, 8> counts = {};
    for (const auto &chunk : model_->chunks) {
      counts[static_cast<size_t>(chunk.status)]++;
    }

    ftxui::Elements summary_parts;
    summary_parts.push_back(chip("󰒓", theme.base.bg, theme.base.highlight));
    summary_parts.push_back(ftxui::text(" "));
    summary_parts.push_back(ftxui::paragraph(model_->plan_title) |
                            ftxui::bold | ftxui::color(theme.base.fg));
    if (counts[static_cast<size_t>(shared::WorkChunkStatus::InProgress)] > 0) {
      summary_parts.push_back(ftxui::text(" "));
      summary_parts.push_back(chip(
          " " +
              std::to_string(
                  counts[static_cast<size_t>(shared::WorkChunkStatus::InProgress)]),
          theme.base.fg, theme.status_bar.streaming.normal.fg));
    }
    if (counts[static_cast<size_t>(shared::WorkChunkStatus::Verifying)] > 0) {
      summary_parts.push_back(ftxui::text(" "));
      summary_parts.push_back(chip(
          " " +
              std::to_string(
                  counts[static_cast<size_t>(shared::WorkChunkStatus::Verifying)]),
          theme.base.fg, theme.status_bar.provider_waiting.normal.fg));
    }
    if (counts[static_cast<size_t>(shared::WorkChunkStatus::Ready)] > 0) {
      summary_parts.push_back(ftxui::text(" "));
      summary_parts.push_back(chip(
          " " +
              std::to_string(
                  counts[static_cast<size_t>(shared::WorkChunkStatus::Ready)]),
          theme.base.fg, theme.base.dim));
    }

    auto summary_row = ftxui::hflow(std::move(summary_parts)) |
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
                        chip(std::string(style.icon) + " " + std::to_string(count),
                             theme.base.fg,
                             statusChipBackground(theme, status)),
                    }) |
                    ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex;
    }

    auto body = scrollable_ ? scrollable_->Render() | ftxui::xflex | ftxui::yflex
                            : ftxui::text("");
    return ftxui::vbox({summary_row, body}) |
           ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex;
  }

private:
  ftxui::Element RenderBody() {
    if (!model_ || !model_->visible) {
      return ftxui::text("");
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    ftxui::Elements rows;

    if (model_->executor_task_view) {
      for (const auto &task : model_->executor_tasks) {
        const auto style = styleFor(theme, task.status);
        const std::string task_label =
            (task.id.empty() ? std::string() : task.id + ". ") + task.title;
        auto line = ftxui::paragraph(task_label) |
            ftxui::color(task.status == shared::WorkChunkStatus::InProgress ||
                                 task.status == shared::WorkChunkStatus::Verifying
                             ? theme.base.highlight
                             : theme.base.fg);
        if (task.status == shared::WorkChunkStatus::InProgress) {
          line = line | ftxui::bold;
        }
        auto row = ftxui::hbox({
                       ftxui::text(" " + std::string(style.icon) + " ") |
                           ftxui::bold | ftxui::color(style.color),
                       line | ftxui::flex,
                   }) |
                   ftxui::xflex;
        if (task.status == shared::WorkChunkStatus::Done) {
          line = line | ftxui::strikethrough;
          row = ftxui::hbox({
                    ftxui::text(" " + std::string(style.icon) + " ") |
                        ftxui::bold | ftxui::color(style.color) | ftxui::dim,
                    line | ftxui::flex,
                }) |
                ftxui::xflex | ftxui::dim;
        }
        rows.push_back(row);
      }

      return ftxui::vbox(std::move(rows)) |
             ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex;
    }

    for (const auto &chunk : model_->chunks) {
      const auto style = styleFor(theme, chunk.status);
      std::string task_hint;
      if (chunk.task_count.has_value() && chunk.task_count.value() > 0) {
        task_hint = " (" + std::to_string(chunk.task_count.value()) + " tasks)";
      }
      const bool is_highlight_chunk =
          !model_->highlight_chunk_id.empty() &&
          model_->highlight_chunk_id == chunk.id;

      auto line =
          ftxui::paragraph(std::string(is_highlight_chunk ? "󰎤 " : "") +
                           chunk.title + task_hint) |
          ftxui::color(chunk.status == shared::WorkChunkStatus::InProgress ||
                               chunk.status == shared::WorkChunkStatus::Verifying
                           ? theme.base.highlight
                           : theme.base.fg);
      if (chunk.status == shared::WorkChunkStatus::InProgress ||
          is_highlight_chunk) {
        line = line | ftxui::bold;
      }
      auto row = ftxui::hbox({
                     ftxui::text(" " + std::string(style.icon) + " ") |
                         ftxui::bold | ftxui::color(style.color),
                     line | ftxui::flex,
                 }) |
                 ftxui::xflex;
      if (is_highlight_chunk) {
        row = row | ftxui::bgcolor(theme.base.bg);
      }
      if (chunk.status == shared::WorkChunkStatus::Done) {
        row = row | ftxui::dim;
      }
      rows.push_back(row);
    }

    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme.agent_strip.bg) |
           ftxui::xflex;
  }

  std::shared_ptr<PlanLaneModel> model_;
  ftxui::Component body_renderer_;
  std::shared_ptr<ScrollableBoxComponent> scrollable_;
};

} // namespace

ftxui::Component PlanLane(const std::shared_ptr<PlanLaneModel> &model) {
  return std::make_shared<PlanLaneComponentBase>(model);
}

} // namespace firmius::tui
