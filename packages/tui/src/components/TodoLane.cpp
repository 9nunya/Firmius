#include "components/TodoLane.hpp"
#include "ThemeManager.hpp"
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
      : model_(std::move(model)) {}

  ftxui::Element OnRender() override {
    if (!model_ || !model_->visible) {
      return ftxui::text("");
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    ftxui::Elements rows;
    rows.push_back(
        ftxui::hbox({
            ftxui::text(" TODO ") | ftxui::bold | ftxui::color(theme.base.bg) |
                ftxui::bgcolor(theme.base.highlight),
            ftxui::text(" " + model_->owner_label) | ftxui::color(theme.base.fg),
        }) |
        ftxui::xflex);

    if (model_->show_chunk_header && !model_->chunk_title.empty()) {
      rows.push_back(ftxui::text("|CHUNK| " + model_->chunk_title) |
                     ftxui::bold | ftxui::color(theme.base.highlight));
    }

    size_t actionableIndex = model_->rows.size();
    for (size_t i = 0; i < model_->rows.size(); ++i) {
      if (model_->rows[i].status != shared::TodoStatus::Done) {
        actionableIndex = i;
        break;
      }
    }

    for (size_t i = 0; i < model_->rows.size(); ++i) {
      const auto &row = model_->rows[i];
      const bool isDone = row.status == shared::TodoStatus::Done;
      const bool isInProgress = row.status == shared::TodoStatus::InProgress;
      const bool isTopActionable = i == actionableIndex;

      auto prefix = ftxui::text((isTopActionable ? "> " : "  ") +
                                std::to_string(row.id) + ". [" +
                                std::string(1, statusMarker(row.status)) + "] ");
      auto content = ftxui::text(row.text);

      if (isDone) {
        content = content | ftxui::dim | ftxui::strikethrough;
      } else if (isInProgress) {
        content = content | ftxui::bold | ftxui::color(theme.base.highlight);
      } else if (isTopActionable) {
        content = content | ftxui::bold;
      }

      rows.push_back(ftxui::hbox({prefix, content | ftxui::flex}) | ftxui::xflex);
    }

    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme.agent_strip.bg) |
           ftxui::xflex;
  }

private:
  std::shared_ptr<TodoLaneModel> model_;
};

ftxui::Component TodoLane(const std::shared_ptr<TodoLaneModel> &model) {
  return std::make_shared<TodoLaneComponentBase>(model);
}

} // namespace firmius::tui
