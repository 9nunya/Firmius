#include "TUIState.hpp"
#include "AgentRegistry.hpp"
#include "components/ChatWindow.hpp"
#include "components/InputBar.hpp"
#include "components/StatusBar.hpp"
#include "components/TitleBar.hpp"
#include "components/ToolBlock.hpp"
#include "components/Markdown.hpp"
#include "harness/Harness.hpp"
#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

static std::string statusToString(shared::AgentStatus status) {
    using shared::AgentStatus;
    switch (status) {
        case AgentStatus::Idle: return "idle";
        case AgentStatus::Streaming: return "streaming";
        case AgentStatus::ExecutingTool: return "executing_tool";
        case AgentStatus::AwaitingInput: return "awaiting_input";
        case AgentStatus::Compacting: return "compacting";
        case AgentStatus::ProviderWaiting: return "provider_waiting";
        case AgentStatus::Error: return "error";
        case AgentStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

TuiState& TuiState::instance() {
    static TuiState inst;
    return inst;
}

TuiState::TuiState() = default;

void TuiState::init(firmius::core::Harness& harness,
                    const shared::ThreadMetadata& thread,
                    const std::string& focused_agent_id) {
    harness_ = &harness;
    thread_ = thread;
    focused_agent_id_ = focused_agent_id;
    history_ = harness_->getAgentHistoryPtr(focused_agent_id_);

    title_model_ = std::make_shared<TitleBarModel>();
    title_model_->title = thread_.title;
    title_model_->thread_id = thread_.threadId;

    status_model_ = std::make_shared<StatusBarModel>();
    status_model_->status_text = "status=unknown";

    input_model_ = std::make_shared<InputBarModel>();
    input_model_->buffer = &input_;
    input_model_->cursor = &cursor_;
    input_model_->placeholder = "Type a message...";

    subscription_id_ = harness_->subscribe([this](const harness::HarnessEvent& ev) {
        this->onEvent(ev);
    });
}

void TuiState::attachScreen(ftxui::ScreenInteractive* screen) {
    screen_ = screen;
}

void TuiState::shutdown() {
    if (harness_ && subscription_id_ >= 0) {
        harness_->unsubscribe(subscription_id_);
        subscription_id_ = -1;
    }
}

void TuiState::onEvent(const harness::HarnessEvent& ev) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (auto* e = std::get_if<harness::MessageChunk>(&ev)) {
            auto& s = streams_[e->agentId];
            if (e->isThinking) {
                s.thinking += e->delta;
            } else {
                s.text += e->delta;
            }
            s.provider_waiting = false;
        } else if (auto* e = std::get_if<harness::MessageCompleted>(&ev)) {
            auto& s = streams_[e->agentId];
            s.thinking.clear();
            s.text.clear();
            s.provider_waiting = false;
            for (auto it = tool_order_.begin(); it != tool_order_.end();) {
                auto it_tool = tool_calls_.find(*it);
                if (it_tool != tool_calls_.end() && it_tool->second &&
                    it_tool->second->agentId == e->agentId) {
                    tool_calls_.erase(it_tool);
                    it = tool_order_.erase(it);
                } else {
                    ++it;
                }
            }
        } else if (auto* e = std::get_if<harness::AgentProviderWaiting>(&ev)) {
            streams_[e->agentId].provider_waiting = true;
        } else if (auto* e = std::get_if<harness::ToolCallArgsChunk>(&ev)) {
            auto& view = tool_calls_[e->toolCallId];
            if (!view) {
                view = std::make_shared<ToolCallView>();
                view->toolCallId = e->toolCallId;
                view->agentId = e->agentId;
                tool_order_.push_back(e->toolCallId);
            }
            view->phase = ToolPhase::Preparing;
            view->name += e->nameDelta;
            view->args += e->delta;
            if (!view->args.empty()) {
                view->phase = ToolPhase::Called;
            }
        } else if (auto* e = std::get_if<harness::ToolCallStarted>(&ev)) {
            auto& view = tool_calls_[e->toolCallId];
            if (!view) {
                view = std::make_shared<ToolCallView>();
                view->toolCallId = e->toolCallId;
                tool_order_.push_back(e->toolCallId);
            }
            view->agentId = e->agentId;
            if (!e->name.empty()) view->name = e->name;
            if (!e->args.empty()) view->args = e->args;
            view->phase = view->args.empty() ? ToolPhase::Preparing : ToolPhase::Called;
        } else if (auto* e = std::get_if<harness::ToolCallResult>(&ev)) {
            auto it_tool = tool_calls_.find(e->toolCallId);
            if (it_tool != tool_calls_.end()) {
                tool_calls_.erase(it_tool);
            }
            tool_order_.erase(
                std::remove(tool_order_.begin(), tool_order_.end(), e->toolCallId),
                tool_order_.end()
            );
        } else if (auto* e = std::get_if<harness::CompactionThinkingChunk>(&ev)) {
            streams_[e->agentId].compaction_thinking += e->delta;
        } else if (auto* e = std::get_if<harness::CompactionTextChunk>(&ev)) {
            streams_[e->agentId].compaction_text += e->delta;
        } else if (auto* e = std::get_if<harness::ContextCompactedEvent>(&ev)) {
            auto& s = streams_[e->agentId];
            s.compaction_thinking.clear();
            s.compaction_text.clear();
        }
    }

    if (screen_) {
        screen_->PostEvent(ftxui::Event::Custom);
    }
}

std::string TuiState::statusText() const {
    std::string status_line = "status=unknown";
    if (!focused_agent_id_.empty()) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
        if (agent) {
            const auto& ctx = agent->getContext();
            status_line = "status=" + statusToString(ctx.state.currentStatus) +
                          " | model=" + ctx.config.providerId + "/" + ctx.config.modelId +
                          " | purpose=" + ctx.identity.role;
        }
    }
    return status_line;
}

ftxui::Component TuiState::root() {
    if (root_component_) return root_component_;

    auto title_bar = TitleBar(title_model_);
    auto status_bar = StatusBar(status_model_);
    auto input_bar = InputBar(input_model_, [this](const std::string& text) {
        if (harness_) harness_->send(text);
    });
    auto chat = ChatWindow(history_);
    chat_component_ = chat;

    auto container = ftxui::Container::Vertical({
        input_bar,
        chat,
    });

    root_component_ = ftxui::Renderer(container, [this, title_bar, status_bar, input_bar, chat] {
        status_model_->status_text = statusText();

        std::vector<ftxui::Element> live_rows;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = streams_.find(focused_agent_id_);
            if (it != streams_.end()) {
                const auto& s = it->second;
                if (!s.thinking.empty()) {
                    live_rows.push_back(ftxui::text("[thinking]") | ftxui::dim);
                    live_rows.push_back(RenderMarkdown(s.thinking, true));
                }
                if (!s.text.empty()) {
                    live_rows.push_back(RenderMarkdown(s.text));
                }
                if (!s.compaction_thinking.empty()) {
                    live_rows.push_back(ftxui::text("[compacting:thinking]") | ftxui::dim);
                    live_rows.push_back(RenderMarkdown(s.compaction_thinking, true));
                }
                if (!s.compaction_text.empty()) {
                    live_rows.push_back(ftxui::text("[compacting]") | ftxui::dim);
                    live_rows.push_back(RenderMarkdown(s.compaction_text, true));
                }
                if (s.provider_waiting) {
                    live_rows.push_back(ftxui::text("[provider waiting]") | ftxui::dim);
                }
            }

            for (const auto& id : tool_order_) {
                auto it_tool = tool_calls_.find(id);
                if (it_tool == tool_calls_.end()) continue;
                const auto& view = it_tool->second;
                if (!view || view->agentId != focused_agent_id_) continue;
                live_rows.push_back(ToolBlock(view)->Render());
            }
        }

        auto history_block = chat->Render() | ftxui::flex;
        if (!live_rows.empty()) {
            live_rows.insert(live_rows.begin(), ftxui::separator());
        }

        auto chat_area = ftxui::vbox({
            history_block,
            ftxui::vbox(std::move(live_rows)),
        }) | ftxui::flex;

        return ftxui::vbox({
            title_bar->Render(),
            chat_area,
            ftxui::separator(),
            input_bar->Render(),
            status_bar->Render(),
        }) | ftxui::flex;
    });

    root_component_ = ftxui::CatchEvent(root_component_, [this, chat](ftxui::Event event) {
        if (event.is_mouse()) {
            auto& m = event.mouse();
            if (m.button == ftxui::Mouse::WheelUp || m.button == ftxui::Mouse::WheelDown) {
                if (chat_component_) {
                    return chat_component_->OnEvent(event);
                }
            }
        }
        if (event == ftxui::Event::PageUp ||
            event == ftxui::Event::PageDown ||
            event == ftxui::Event::Home ||
            event == ftxui::Event::End) {
            if (chat_component_) {
                return chat_component_->OnEvent(event);
            }
        }
        if (event == ftxui::Event::Escape) {
            if (harness_) harness_->abort();
            return true;
        }
        return false;
    });

    return root_component_;
}

}
