#include "components/TurnBlock.hpp"
#include "Message.hpp"
#include "components/ToolBlock.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <variant>

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

ftxui::Element renderToolCall(const firmius::shared::ToolCallContent& tc) {
    auto view = std::make_shared<firmius::tui::ToolCallView>();
    view->toolCallId = tc.id;
    view->name = tc.name;
    view->args = tc.args;
    view->phase = firmius::tui::ToolPhase::Called;
    return firmius::tui::ToolBlock(view)->Render();
}

ftxui::Element renderToolResult(const firmius::shared::ToolResultContent& tr) {
    auto view = std::make_shared<firmius::tui::ToolCallView>();
    view->toolCallId = tr.toolCallId;
    view->name = tr.toolCallId;
    view->result = tr.result;
    view->success = tr.success;
    view->phase = firmius::tui::ToolPhase::Finished;
    return firmius::tui::ToolBlock(view)->Render();
}

namespace firmius::tui {

    ftxui::Component TurnBlock(const shared::AgentTurn &turn) {
        return ftxui::Renderer([turn] {
            std::vector<ftxui::Element> rows;

            for (const auto& msg : turn.messages) {
                std::vector<ftxui::Element> parts;

                for (const auto& part : msg.content) {
                    if (auto* txt = std::get_if<shared::TextContent>(&part)) {
                        parts.push_back(ftxui::paragraph(txt->text));
                    } else if (auto* thk = std::get_if<shared::ThinkingContent>(&part)) {
                        parts.push_back(ftxui::paragraph(thk->thinking) | ftxui::dim);
                    } else if (auto* tc = std::get_if<shared::ToolCallContent>(&part)) {
                        parts.push_back(renderToolCall(*tc));
                    } else if (auto* tr = std::get_if<shared::ToolResultContent>(&part)) {
                        parts.push_back(renderToolResult(*tr));
                    }
                }

                if (parts.empty()) {
                    parts.push_back(ftxui::text(""));
                }

                auto content_block = ftxui::vbox(std::move(parts));
                rows.push_back(
                    ftxui::hbox(
                        ftxui::text(rolePrefix(msg.role)) | ftxui::bold,
                        content_block | ftxui::flex
                    )
                );
            }

            return ftxui::vbox(std::move(rows));
        });
    }

}
