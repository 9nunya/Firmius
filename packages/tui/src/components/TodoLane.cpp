#include "components/TodoLane.hpp"
#include "ThemeManager.hpp"
#include "components/ScrollableBox.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

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

} // namespace

class TodoLaneComponentBase : public ftxui::ComponentBase {
public:
  explicit TodoLaneComponentBase(std::shared_ptr<TodoLaneModel> model)
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
    ftxui::Elements rows;
    rows.push_back(
        ftxui::hbox({
            ftxui::text(" 󰄬 ") | ftxui::bold | ftxui::color(theme.base.bg) |
                ftxui::bgcolor(theme.base.highlight),
            ftxui::paragraph(" " + model_->owner_label) |
                ftxui::color(theme.base.fg),
        }) |
        ftxui::xflex);

    if (model_->show_chunk_header && !model_->chunk_title.empty()) {
      rows.push_back(ftxui::paragraph("󰆧 " + model_->chunk_title) |
                     ftxui::bold | ftxui::color(theme.base.highlight));
    }

    auto body = scrollable_ ? scrollable_->Render() | ftxui::xflex | ftxui::yflex
                            : ftxui::text("");
    rows.push_back(body);
    return ftxui::vbox(std::move(rows)) |
           ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex;
  }

private:
  ftxui::Element RenderBody() {
    if (!model_ || !model_->visible) {
      return ftxui::text("");
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    size_t actionableIndex = model_->rows.size();
    for (size_t i = 0; i < model_->rows.size(); ++i) {
      if (model_->rows[i].status != shared::TodoStatus::Done) {
        actionableIndex = i;
        break;
      }
    }

    ftxui::Elements rows;
    for (size_t i = 0; i < model_->rows.size(); ++i) {
      const auto &row = model_->rows[i];
      const bool isDone = row.status == shared::TodoStatus::Done;
      const bool isInProgress = row.status == shared::TodoStatus::InProgress;
      const bool isTopActionable = i == actionableIndex;

      auto content = ftxui::paragraph((isTopActionable ? "> " : "  ") +
                                      std::to_string(row.id) + ". [" +
                                      std::string(1, statusMarker(row.status)) +
                                      "] " + row.text);

      if (isDone) {
        content = content | ftxui::dim | ftxui::strikethrough;
      } else if (isInProgress) {
        content = content | ftxui::bold | ftxui::color(theme.base.highlight);
      } else if (isTopActionable) {
        content = content | ftxui::bold;
      }

      rows.push_back(content | ftxui::xflex);
    }

    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme.agent_strip.bg) |
           ftxui::xflex;
  }

  std::shared_ptr<TodoLaneModel> model_;
  ftxui::Component body_renderer_;
  std::shared_ptr<ScrollableBoxComponent> scrollable_;
};

ftxui::Component TodoLane(const std::shared_ptr<TodoLaneModel> &model) {
  return std::make_shared<TodoLaneComponentBase>(model);
}

} // namespace firmius::tui
