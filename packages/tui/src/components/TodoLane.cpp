#include "components/TodoLane.hpp"
#include "ThemeManager.hpp"
#include "components/ScrollableBox.hpp"
#include "utils/Icons.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>

namespace firmius::tui {

namespace {

char statusMarker(shared::TodoStatus status) {
  switch (status) {
  case shared::TodoStatus::Pending:
    return ' ';
  case shared::TodoStatus::InProgress:
    return '*';
  case shared::TodoStatus::Done:
    return 'x';
  }
  return ' ';
}

size_t focusRowIndex(const TodoLaneModel &model) {
  for (size_t i = 0; i < model.rows.size(); ++i) {
    if (model.rows[i].status == shared::TodoStatus::InProgress) {
      return i;
    }
  }
  for (size_t i = 0; i < model.rows.size(); ++i) {
    if (model.rows[i].status != shared::TodoStatus::Done) {
      return i;
    }
  }
  return model.rows.empty() ? 0 : model.rows.size() - 1;
}

ftxui::Element buildTodoRowElement(const Theme &theme, const TodoLaneRow &row,
                                   bool isFocusRow) {
  const bool isDone = row.status == shared::TodoStatus::Done;
  const bool isInProgress = row.status == shared::TodoStatus::InProgress;
  const std::string prefix = (isFocusRow ? "> " : "  ") +
                             std::to_string(row.id) + ". [" +
                             std::string(1, statusMarker(row.status)) + "] ";

  auto prefix_element = ftxui::text(prefix);
  auto text_element = ftxui::paragraph(row.text);

  if (isDone) {
    prefix_element = prefix_element | ftxui::dim;
    text_element = text_element | ftxui::dim | ftxui::strikethrough;
  } else if (isInProgress) {
    prefix_element =
        prefix_element | ftxui::bold | ftxui::color(theme.base.highlight);
    text_element =
        text_element | ftxui::bold | ftxui::color(theme.base.highlight);
  } else if (isFocusRow) {
    prefix_element = prefix_element | ftxui::bold;
    text_element = text_element | ftxui::bold;
  }

  return ftxui::hbox({
             prefix_element,
             text_element | ftxui::flex,
         }) |
         ftxui::xflex;
}

} // namespace

class TodoLaneComponentBase : public ftxui::ComponentBase {
public:
  explicit TodoLaneComponentBase(std::shared_ptr<TodoLaneModel> model)
      : model_(std::move(model)) {
    body_renderer_ = ftxui::Renderer([this] { return RenderBody(); });
    scrollable_ = ScrollableBox(body_renderer_, {.startAtBottom = false,
                                                 .overlayScrollbar = true,
                                                 .showScrollbar = false});
    Add(scrollable_);
  }

  ftxui::Element OnRender() override {
    if (!model_ || !model_->visible) {
      return ftxui::text("");
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    maybeAutoScrollToFocusRow(theme);
    ftxui::Elements rows;
    rows.push_back(ftxui::hbox({
                       ftxui::text(" " + shared::ICON_TODO + " ") |
                           ftxui::bold | ftxui::color(theme.base.bg) |
                           ftxui::bgcolor(theme.base.highlight),
                       ftxui::paragraph(" " + model_->owner_label) |
                           ftxui::color(theme.base.fg),
                       ftxui::text(model_->toggle_hint) | ftxui::dim,
                   }) |
                   ftxui::xflex);
    if (model_->show_chunk_header && !model_->chunk_title.empty()) {
      rows.push_back(ftxui::paragraph("󰆧 " + model_->chunk_title) |
                     ftxui::bold | ftxui::color(theme.base.highlight));
    }

    auto body = scrollable_
                    ? scrollable_->Render() | ftxui::xflex | ftxui::yflex
                    : ftxui::text("");
    rows.push_back(body);
    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme.agent_strip.bg) |
           ftxui::xflex;
  }

private:
  void maybeAutoScrollToFocusRow(const Theme &theme) {
    if (!model_ || !scrollable_ || model_->rows.empty()) {
      return;
    }

    const auto focus_index = static_cast<int>(focusRowIndex(*model_));
    const int content_width = scrollable_->ContentWidth();
    if (focus_index == last_auto_scroll_focus_index_ &&
        content_width == last_auto_scroll_content_width_) {
      return;
    }

    if (content_width <= 1) {
      scrollable_->RequestEnsureVisible(focus_index);
    } else {
      int line_start = 0;
      for (int i = 0; i < focus_index; ++i) {
        auto row_element =
            buildTodoRowElement(theme, model_->rows[static_cast<size_t>(i)],
                                static_cast<size_t>(i) ==
                                    focusRowIndex(*model_)) |
            ftxui::size(ftxui::WIDTH, ftxui::EQUAL, content_width);
        const auto measured = ftxui::Dimension::Fit(row_element, true);
        line_start += std::max(1, measured.dimy);
      }

      auto focus_element =
          buildTodoRowElement(
              theme, model_->rows[static_cast<size_t>(focus_index)], true) |
          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, content_width);
      const auto measured_focus = ftxui::Dimension::Fit(focus_element, true);
      const int line_end = line_start + std::max(1, measured_focus.dimy) - 1;
      scrollable_->RequestEnsureVisible(line_start, line_end);
    }

    last_auto_scroll_focus_index_ = focus_index;
    last_auto_scroll_content_width_ = content_width;
  }

  ftxui::Element RenderBody() {
    if (!model_ || !model_->visible) {
      return ftxui::text("");
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const size_t actionableIndex =
        model_->rows.empty() ? 0 : focusRowIndex(*model_);

    ftxui::Elements rows;
    for (size_t i = 0; i < model_->rows.size(); ++i) {
      const bool isTopActionable = i == actionableIndex;
      rows.push_back(
          buildTodoRowElement(theme, model_->rows[i], isTopActionable));
    }

    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme.agent_strip.bg) |
           ftxui::xflex;
  }

  std::shared_ptr<TodoLaneModel> model_;
  ftxui::Component body_renderer_;
  std::shared_ptr<ScrollableBoxComponent> scrollable_;
  int last_auto_scroll_focus_index_ = -1;
  int last_auto_scroll_content_width_ = -1;
};

ftxui::Component TodoLane(const std::shared_ptr<TodoLaneModel> &model) {
  return std::make_shared<TodoLaneComponentBase>(model);
}

} // namespace firmius::tui
