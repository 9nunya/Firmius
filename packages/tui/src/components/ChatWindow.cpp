#include "components/ChatWindow.hpp"
#include "Context.hpp"
#include "Enums.hpp"
#include "Message.hpp"
#include "components/ToolBlock.hpp"
#include "components/ScrollableBox.hpp"
#include "components/Markdown.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <memory>
#include <vector>
#include <unordered_map>

bool isSystemTurn(firmius::shared::AgentTurn turn) {
    return turn.messages.front().role == firmius::shared::Role::System;
}

static std::string rolePrefix(firmius::shared::Role role) {
    using firmius::shared::Role;
    switch (role) {
        case Role::User: return "> ";
        case Role::Assistant: return "* ";
        case Role::System: return "# ";
        case Role::ToolResult: return "+ ";
    }
    return "? ";
}

ftxui::Element prefixedRow(const std::string& prefix, const ftxui::Element& content) {
    return ftxui::hbox(
        ftxui::text(prefix) | ftxui::bold,
        content | ftxui::flex
    );
}

namespace {

class ChatWindowComponent : public ftxui::ComponentBase {
public:
    explicit ChatWindowComponent(std::shared_ptr<firmius::shared::AgentHistory> history)
        : history_(std::move(history)) {
        container_ = ftxui::Container::Vertical({});
        scrollable_ = firmius::tui::ScrollableBox(container_);
        Add(scrollable_);
    }

    ftxui::Element Render() override {
        RebuildIfNeeded();
        return scrollable_ ? scrollable_->Render() : ftxui::text("");
    }

    bool OnEvent(ftxui::Event event) override {
        RebuildIfNeeded();
        return scrollable_ ? scrollable_->OnEvent(event) : false;
    }

private:
    class RowComponent : public ftxui::ComponentBase {
    public:
        RowComponent(ftxui::Component child, std::function<ftxui::Element()> render)
            : child_(std::move(child)), render_(std::move(render)) {
            if (child_) Add(child_);
        }

        ftxui::Element Render() override { return render_(); }
        bool Focusable() const override { return true; }
        bool OnEvent(ftxui::Event event) override {
            if (child_) return child_->OnEvent(event);
            return false;
        }

    private:
        ftxui::Component child_;
        std::function<ftxui::Element()> render_;
    };

    void RebuildIfNeeded() {
        size_t turns_size = history_ ? history_->turns.size() : 0;
        if (turns_size == last_turns_size_) return;
        last_turns_size_ = turns_size;

        rows_.clear();
        container_->DetachAllChildren();
        tail_spacer_ = ftxui::Make<RowComponent>(nullptr, [] { return ftxui::text(""); });

        if (history_) {
            std::unordered_map<std::string, bool> seen_tool_call;
            for (const auto& t : history_->turns) {
                if (t.messages.empty()) continue;
                if (isSystemTurn(t)) continue;

                for (const auto& msg : t.messages) {
                    const std::string prefix = rolePrefix(msg.role);

                    for (const auto& part : msg.content) {
                        if (auto* txt = std::get_if<firmius::shared::TextContent>(&part)) {
                            auto row = ftxui::Make<RowComponent>(nullptr, [prefix, txt] {
                                return prefixedRow(prefix, firmius::tui::RenderMarkdown(txt->text));
                            });
                            rows_.push_back(row);
                        } else if (auto* thk = std::get_if<firmius::shared::ThinkingContent>(&part)) {
                            auto row = ftxui::Make<RowComponent>(nullptr, [prefix, thk] {
                                return prefixedRow(prefix, firmius::tui::RenderMarkdown(thk->thinking, true));
                            });
                            rows_.push_back(row);
                        } else if (auto* tc = std::get_if<firmius::shared::ToolCallContent>(&part)) {
                            auto& view = tool_views_[tc->id];
                            if (!view) view = std::make_shared<firmius::tui::ToolCallView>();
                            view->toolCallId = tc->id;
                            view->name = tc->name;
                            view->args = tc->args;
                            view->phase = firmius::tui::ToolPhase::Called;
                            seen_tool_call[tc->id] = true;

                            auto block = ToolBlock(view);
                            auto row = ftxui::Make<RowComponent>(block, [prefix, block] {
                                return prefixedRow(prefix, block->Render());
                            });
                            rows_.push_back(row);
                        } else if (auto* tr = std::get_if<firmius::shared::ToolResultContent>(&part)) {
                            auto& view = tool_views_[tr->toolCallId];
                            if (!view) view = std::make_shared<firmius::tui::ToolCallView>();
                            view->toolCallId = tr->toolCallId;
                            view->result = tr->result;
                            view->success = tr->success;
                            view->phase = firmius::tui::ToolPhase::Finished;

                            if (!seen_tool_call[tr->toolCallId]) {
                                auto block = ToolBlock(view);
                                auto row = ftxui::Make<RowComponent>(block, [prefix, block] {
                                    return prefixedRow(prefix, block->Render());
                                });
                                rows_.push_back(row);
                                seen_tool_call[tr->toolCallId] = true;
                            }
                        }
                    }
                }
            }
        }

        for (auto& row : rows_) container_->Add(row);
        // Bottom margin inside the scrollable area.
        container_->Add(tail_spacer_);
        // Keep view anchored to bottom on new content.
        if (scrollable_) {
            scrollable_->RequestScrollToBottom();
            scrollable_->SetContentLength(static_cast<int>(rows_.size()) + 1);
        }
    }

    std::shared_ptr<firmius::shared::AgentHistory> history_;
    size_t last_turns_size_ = 0;
    std::vector<ftxui::Component> rows_;
    ftxui::Component container_;
    ftxui::Component tail_spacer_;
    std::shared_ptr<firmius::tui::ScrollableBoxComponent> scrollable_;
    std::unordered_map<std::string, std::shared_ptr<firmius::tui::ToolCallView>> tool_views_;
};

} // namespace

ftxui::Component firmius::tui::ChatWindow(const std::shared_ptr<shared::AgentHistory> &history) {
    return ftxui::Make<ChatWindowComponent>(history);
}
