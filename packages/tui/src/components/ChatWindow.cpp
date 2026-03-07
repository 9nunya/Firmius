#include "components/ChatWindow.hpp"
#include "Context.hpp"
#include "Enums.hpp"
#include "Message.hpp"
#include "components/Markdown.hpp"
#include "components/ScrollableBox.hpp"
#include "components/ToolBlock.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

bool isSystemTurn(const firmius::shared::AgentTurn &turn) {
  return turn.messages.front().role == firmius::shared::Role::System;
}

static std::string rolePrefix(firmius::shared::Role role) {
  using firmius::shared::Role;
  switch (role) {
  case Role::User:
    return "> ";
  case Role::Assistant:
    return "* ";
  case Role::System:
    return "# ";
  case Role::ToolResult:
    return "+ ";
  }
  return "? ";
}

namespace {

class RowComponent : public ftxui::ComponentBase {
public:
  RowComponent(ftxui::Component child, std::function<ftxui::Element()> render)
      : child_(std::move(child)), render_(std::move(render)) {
    if (child_)
      Add(child_);
  }

  ftxui::Element Render() override { return render_(); }
  bool Focusable() const override { return true; }
  bool OnEvent(ftxui::Event event) override {
    if (child_)
      return child_->OnEvent(event);
    return false;
  }

private:
  ftxui::Component child_;
  std::function<ftxui::Element()> render_;
};

class ChatWindowComponent : public ftxui::ComponentBase {
public:
  explicit ChatWindowComponent(
      std::function<const firmius::shared::AgentHistory*()> history_getter,
      std::function<std::vector<ftxui::Element>()> live_rows_provider)
      : history_getter_(std::move(history_getter)),
        live_rows_provider_(std::move(live_rows_provider)) {

    history_container_ = ftxui::Container::Vertical({});

    auto live_rows_cmp = ftxui::Make<RowComponent>(nullptr, [this] {
      if (!live_rows_provider_)
        return ftxui::text("");
      auto rows = live_rows_provider_();
      if (rows.empty())
        return ftxui::text("");

      bool prepend_gap = false;
      auto* history = history_getter_ ? history_getter_() : nullptr;
      if (history && !history->turns.empty()) {
        const auto &last_turn = history->turns.back();
        if (!last_turn.messages.empty()) {
          if (last_turn.messages.front().role == firmius::shared::Role::User) {
            prepend_gap = true;
          }
        }
      }

      if (prepend_gap) {
        rows.insert(rows.begin(),
                    ftxui::text("") |
                        ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1));
      }

      return ftxui::vbox(std::move(rows));
    });

    tail_spacer_ =
        ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); });

    container_ = ftxui::Container::Vertical(
        {history_container_, live_rows_cmp, tail_spacer_});

    scrollable_ = firmius::tui::ScrollableBox(container_);
    Add(scrollable_);
  }

  ftxui::Element Render() override {
    RebuildIfNeeded();
    return scrollable_ ? scrollable_->Render() : ftxui::text("");
  }

  bool OnEvent(ftxui::Event event) override {
    if (event == ftxui::Event::Special("ThreadChanged")) {
      last_turns_size_ = static_cast<size_t>(-1);
      RebuildIfNeeded();
      user_scrolled_up_ = false;
      return true;
    }

    RebuildIfNeeded();

    if (event == ftxui::Event::ArrowUp || event == ftxui::Event::PageUp) {
      user_scrolled_up_ = true;
    }

    if (event == ftxui::Event::ArrowDown || event == ftxui::Event::PageDown) {
      user_scrolled_up_ = false;
    }

    if (event == ftxui::Event::Custom) {
      if (scrollable_ && !user_scrolled_up_)
        scrollable_->RequestScrollToBottom();
    }
    return scrollable_ ? scrollable_->OnEvent(event) : false;
  }

private:
  bool user_scrolled_up_ = false;
  void RebuildIfNeeded() {
    auto* history = history_getter_ ? history_getter_() : nullptr;
    size_t turns_size = history ? history->turns.size() : 0;
    if (turns_size == last_turns_size_)
      return;
    last_turns_size_ = turns_size;

    rows_.clear();
    history_container_->DetachAllChildren();

    if (history) {
      std::unordered_map<std::string, bool> seen_tool_call;
      bool first_turn = true;
      for (const auto &t : history->turns) {
        if (t.messages.empty() || isSystemTurn(t))
          continue;

        if (!first_turn) {
          // Space between turns
          rows_.push_back(ftxui::Make<RowComponent>(nullptr, [] {
            return ftxui::text("") |
                   ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1);
          }));
        }
        first_turn = false;

        for (const auto &msg : t.messages) {
          const std::string prefix = rolePrefix(msg.role);
          bool isUser = (msg.role == firmius::shared::Role::User);
          ftxui::Color prefixColor =
              isUser ? ftxui::Color::Cyan : ftxui::Color::Yellow;

          auto decorateMsg = [prefixColor,
                              prefix](const ftxui::Element &content) {
            auto e = ftxui::hbox(
                {ftxui::text(prefix) | ftxui::bold | ftxui::color(prefixColor),
                 content | ftxui::flex});
            return e | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 120);
          };

          for (const auto &part : msg.content) {
            if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
              auto row = ftxui::Make<RowComponent>(nullptr, [decorateMsg, txt] {
                return decorateMsg(firmius::tui::RenderMarkdown(txt->text));
              });
              rows_.push_back(row);
            } else if (auto *thk =
                           std::get_if<firmius::shared::ThinkingContent>(
                               &part)) {
              auto row = ftxui::Make<RowComponent>(nullptr, [decorateMsg, thk] {
                return decorateMsg(
                    firmius::tui::RenderMarkdown(thk->thinking, true));
              });
              rows_.push_back(row);
            } else if (auto *tc = std::get_if<firmius::shared::ToolCallContent>(
                           &part)) {
              auto &view = tool_views_[tc->id];
              if (!view)
                view = std::make_shared<firmius::tui::ToolCallView>();
              view->toolCallId = tc->id;
              view->name = tc->name;
              view->args = tc->args;
              view->phase = firmius::tui::ToolPhase::Called;
              seen_tool_call[tc->id] = true;

              auto block = ToolBlock(view);
              auto row = ftxui::Make<RowComponent>(block, [decorateMsg, block] {
                return decorateMsg(block->Render());
              });
              rows_.push_back(row);
            } else if (auto *tr =
                           std::get_if<firmius::shared::ToolResultContent>(
                               &part)) {
              auto &view = tool_views_[tr->toolCallId];
              if (!view)
                view = std::make_shared<firmius::tui::ToolCallView>();
              view->toolCallId = tr->toolCallId;
              view->result = tr->result;
              view->success = tr->success;
              view->phase = firmius::tui::ToolPhase::Finished;

              if (!seen_tool_call[tr->toolCallId]) {
                auto block = ToolBlock(view);
                auto row =
                    ftxui::Make<RowComponent>(block, [decorateMsg, block] {
                      return decorateMsg(block->Render());
                    });
                rows_.push_back(row);
                seen_tool_call[tr->toolCallId] = true;
              }
            }
          }
        }
      }
    }

    for (auto &row : rows_)
      history_container_->Add(row);

    if (scrollable_) {
      scrollable_->RequestScrollToBottom();
    }
  }

  std::function<const firmius::shared::AgentHistory*()> history_getter_;
  std::function<std::vector<ftxui::Element>()> live_rows_provider_;
  size_t last_turns_size_ = 0;
  std::vector<ftxui::Component> rows_;
  ftxui::Component history_container_;
  ftxui::Component container_;
  ftxui::Component tail_spacer_;
  std::shared_ptr<firmius::tui::ScrollableBoxComponent> scrollable_;
  std::unordered_map<std::string, std::shared_ptr<firmius::tui::ToolCallView>>
      tool_views_;
};

} // namespace

ftxui::Component firmius::tui::ChatWindow(
    std::function<const shared::AgentHistory*()> history_getter,
    std::function<std::vector<ftxui::Element>()> live_rows_provider) {
  return ftxui::Make<ChatWindowComponent>(std::move(history_getter),
                                          std::move(live_rows_provider));
}
