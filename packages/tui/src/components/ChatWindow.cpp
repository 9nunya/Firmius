#include "components/ChatWindow.hpp"
#include "Context.hpp"
#include "Enums.hpp"
#include "Message.hpp"
#include "ThemeManager.hpp"
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
  if (turn.messages.empty())
    return false;
  for (const auto &msg : turn.messages) {
    if (msg.role != firmius::shared::Role::System)
      return false;
  }
  return true;
}

static std::string rolePrefix(firmius::shared::Role role) {
  using firmius::shared::Role;
  switch (role) {
  case Role::User:
    return "> ";
  case Role::Assistant:
    return "";
  case Role::System:
    return "# ";
  case Role::ToolResult:
    return "+ ";
  case Role::Error:
    return "! ";
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

  ftxui::Element OnRender() override { return render_(); }
  bool Focusable() const override { return false; }
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
      std::function<const firmius::shared::AgentHistory *()> history_getter,
      std::function<std::vector<ftxui::Element>()> live_rows_provider,
      firmius::tui::ToolViewProvider tool_view_provider,
      firmius::tui::HistoryGetter sub_history_getter,
      firmius::tui::StreamGetter sub_stream_getter)
      : history_getter_(std::move(history_getter)),
        live_rows_provider_(std::move(live_rows_provider)),
        tool_view_provider_(std::move(tool_view_provider)),
        sub_history_getter_(std::move(sub_history_getter)),
        sub_stream_getter_(std::move(sub_stream_getter)) {

    history_inner_ = ftxui::Container::Vertical({});
    history_container_ = ftxui::Renderer(history_inner_, [this] {
      ftxui::Elements elements;
      for (size_t i = 0; i < history_inner_->ChildCount(); ++i) {
        elements.push_back(history_inner_->ChildAt(i)->Render());
      }
      return ftxui::vbox(std::move(elements));
    });

    auto live_rows_cmp = ftxui::Make<RowComponent>(nullptr, [this] {
      if (!live_rows_provider_)
        return ftxui::text("");
      auto rows = live_rows_provider_();
      if (rows.empty())
        return ftxui::text("");

      return ftxui::vbox(std::move(rows));
    });

    tail_spacer_ =
        ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); });

    container_ = ftxui::Container::Vertical(
        {history_container_, live_rows_cmp, tail_spacer_});
    container_ = ftxui::Renderer(container_, [this, live_rows_cmp] {
      return ftxui::vbox({
          history_container_->Render(),
          live_rows_cmp->Render(),
          tail_spacer_->Render(),
      });
    });

    scrollable_ = firmius::tui::ScrollableBox(container_);
    Add(scrollable_);
  }

  ftxui::Element OnRender() override {
    RebuildIfNeeded();
    return scrollable_ ? scrollable_->Render() : ftxui::text("");
  }

  bool Focusable() const override { return false; }

  bool OnEvent(ftxui::Event event) override {
    if (event == ftxui::Event::Special("ThreadChanged") ||
        event == ftxui::Event::Special("ThemeChanged")) {
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
    auto *history = history_getter_ ? history_getter_() : nullptr;
    size_t turns_size = history ? history->turns.size() : 0;
    if (turns_size == last_turns_size_)
      return;
    last_turns_size_ = turns_size;

    rows_.clear();
    history_inner_->DetachAllChildren();

    if (history) {
      std::unordered_map<std::string, bool> seen_tool_call;
      for (const auto &t : history->turns) {
        if (t.messages.empty() || isSystemTurn(t))
          continue;

        for (const auto &msg : t.messages) {
          const auto &theme =
              firmius::tui::ThemeManager::instance().getCurrentTheme();
          const std::string prefix = rolePrefix(msg.role);
          bool isUser = (msg.role == firmius::shared::Role::User);
          ftxui::Color prefixColor =
              isUser ? theme.chat.user_prefix : theme.chat.agent_prefix;

          auto decorateMsg = [prefixColor,
                              prefix](const ftxui::Element &content) {
            auto e = ftxui::hbox({
                ftxui::text(prefix) | ftxui::bold | ftxui::color(prefixColor),
                content | ftxui::flex,
            });
            return e | ftxui::flex;
          };

          for (const auto &part : msg.content) {
            if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
              std::string text = txt->text;
              auto row = ftxui::Make<RowComponent>(
                  nullptr, [decorateMsg, text = std::move(text)] {
                    return decorateMsg(firmius::tui::RenderMarkdown(text));
                  });
              rows_.push_back(row);
            } else if (auto *thk =
                           std::get_if<firmius::shared::ThinkingContent>(
                               &part)) {
              std::string thinking = thk->thinking;
              auto row = ftxui::Make<RowComponent>(
                  nullptr, [decorateMsg, thinking = std::move(thinking)] {
                    return decorateMsg(
                        firmius::tui::RenderMarkdown(thinking, true));
                  });
              rows_.push_back(row);
            } else if (auto *tc = std::get_if<firmius::shared::ToolCallContent>(
                           &part)) {
              auto &view = tool_views_[tc->id];
              if (!view && tool_view_provider_)
                view = tool_view_provider_(tc->id);
              if (!view)
                view = std::make_shared<firmius::tui::ToolCallView>();
              view->toolCallId = tc->id;
              view->name = tc->name;
              view->args = tc->args;
              if (view->phase != firmius::tui::ToolPhase::Finished)
                view->phase = firmius::tui::ToolPhase::Called;
              seen_tool_call[tc->id] = true;

              auto block = firmius::tui::ToolBlock(view);
              auto row = ftxui::Make<RowComponent>(block, [decorateMsg, block] {
                return decorateMsg(block->Render());
              });
              rows_.push_back(row);
            } else if (auto *tr =
                           std::get_if<firmius::shared::ToolResultContent>(
                               &part)) {
              auto &view = tool_views_[tr->toolCallId];
              if (!view && tool_view_provider_)
                view = tool_view_provider_(tr->toolCallId);
              if (!view)
                view = std::make_shared<firmius::tui::ToolCallView>();
              view->toolCallId = tr->toolCallId;
              view->result = tr->result;
              view->success = tr->success;
              view->phase = firmius::tui::ToolPhase::Finished;

              if (!seen_tool_call[tr->toolCallId]) {
                auto block = firmius::tui::ToolBlock(view);
                auto row =
                    ftxui::Make<RowComponent>(block, [decorateMsg, block] {
                      return decorateMsg(block->Render());
                    });
                rows_.push_back(row);
                seen_tool_call[tr->toolCallId] = true;
              }
            } else if (auto *err =
                           std::get_if<firmius::shared::ErrorContent>(&part)) {
              std::string title =
                  "[!] " + err->errorName + ": " + err->description;
              std::string details = err->details;

              auto details_cmp = ftxui::Renderer([details, theme] {
                std::stringstream ss(details);
                std::string line;
                ftxui::Elements lines;
                while (std::getline(ss, line)) {
                  lines.push_back(ftxui::text(line));
                }
                return ftxui::vbox(std::move(lines)) |
                       ftxui::color(theme.status_bar.error.normal.fg);
              });

              auto coll_cmp = ftxui::Collapsible(title, details_cmp);
              auto row = ftxui::Make<RowComponent>(
                  coll_cmp, [decorateMsg, coll_cmp, theme] {
                    return decorateMsg(
                        coll_cmp->Render() |
                        ftxui::color(theme.status_bar.error.normal.fg));
                           });
              rows_.push_back(row);
            } else if (std::holds_alternative<firmius::shared::ImageContent>(
                           part)) {
              std::string indicator = "[Image]";
              auto row = ftxui::Make<RowComponent>(
                  nullptr, [decorateMsg, indicator] {
                    return decorateMsg(ftxui::text(indicator));
                  });
              rows_.push_back(row);
            }
          }
        }
      }
    }

    for (auto &row : rows_)
      history_inner_->Add(row);

    if (history_inner_->ChildCount() == 0) {
      history_inner_->Add(
          ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); }));
    }

    if (scrollable_) {
      scrollable_->RequestScrollToBottom();
    }
  }

  std::function<const firmius::shared::AgentHistory *()> history_getter_;
  std::function<std::vector<ftxui::Element>()> live_rows_provider_;
  firmius::tui::ToolViewProvider tool_view_provider_;
  firmius::tui::HistoryGetter sub_history_getter_;
  firmius::tui::StreamGetter sub_stream_getter_;
  size_t last_turns_size_ = static_cast<size_t>(-1);
  std::vector<ftxui::Component> rows_;
  ftxui::Component history_inner_;
  ftxui::Component history_container_;
  ftxui::Component container_;
  ftxui::Component tail_spacer_;
  std::shared_ptr<firmius::tui::ScrollableBoxComponent> scrollable_;
  std::unordered_map<std::string, std::shared_ptr<firmius::tui::ToolCallView>>
      tool_views_;
};

} // namespace

ftxui::Component firmius::tui::ChatWindow(
    std::function<const shared::AgentHistory *()> history_getter,
    std::function<std::vector<ftxui::Element>()> live_rows_provider,
    ToolViewProvider tool_view_provider,
    firmius::tui::HistoryGetter sub_history_getter,
    firmius::tui::StreamGetter sub_stream_getter) {
  return ftxui::Make<ChatWindowComponent>(
      std::move(history_getter), std::move(live_rows_provider),
      std::move(tool_view_provider), std::move(sub_history_getter),
      std::move(sub_stream_getter));
}
