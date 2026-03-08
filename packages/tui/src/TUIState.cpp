#include "TUIState.hpp"
#include "AgentRegistry.hpp"
#include "commands/CommandManager.hpp"
#include "components/ChatWindow.hpp"
#include "components/InputBar.hpp"
#include "components/Markdown.hpp"
#include "components/StatusBar.hpp"
#include "components/TitleBar.hpp"
#include "components/ToolBlock.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalRegistry.hpp"
#include "modals/ThreadLockedModal.hpp"
#include <algorithm>
#include <filesystem>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

using namespace firmius::shared;

static std::string statusToString(shared::AgentStatus status) {
  using shared::AgentStatus;
  switch (status) {
  case AgentStatus::Idle:
    return "idle";
  case AgentStatus::Streaming:
    return "streaming";
  case AgentStatus::ExecutingTool:
    return "executing_tool";
  case AgentStatus::AwaitingInput:
    return "awaiting_input";
  case AgentStatus::Compacting:
    return "compacting";
  case AgentStatus::ProviderWaiting:
    return "provider_waiting";
  case AgentStatus::Error:
    return "error";
  case AgentStatus::Cancelled:
    return "cancelled";
  }
  return "unknown";
}

TuiState &TuiState::instance() {
  static TuiState inst;
  return inst;
}

TuiState::TuiState() = default;

void TuiState::setViewMode(ViewMode mode) { view_mode_ = mode; }

TuiState::ViewMode TuiState::getViewMode() const { return view_mode_; }

void TuiState::openModal(const std::string &name) {
  firmius::tui::ModalRegistry::instance().openModal(name, *this);
}

void TuiState::openModalDirect(ftxui::Component modal) {
  modals_.push_back(modal);
}

void TuiState::popModal() {
  if (!modals_.empty()) {
    modals_.pop_back();
  }
}

void TuiState::clearModals() { modals_.clear(); }

void TuiState::postEvent(ftxui::Event event) {
  if (screen_) {
    screen_->PostEvent(event);
  }
}

void TuiState::init(firmius::core::Harness &harness,
                    const shared::ThreadMetadata &thread,
                    const std::string &focused_agent_id) {
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

  subscription_id_ =
      harness_->subscribe([this](const firmius::shared::AppEvent &ev) {
        event_queue_.push(ev);
        if (screen_) {
          screen_->PostEvent(ftxui::Event::Custom);
        }
      });
}

void TuiState::attachScreen(ftxui::ScreenInteractive *screen) {
  screen_ = screen;
}

void TuiState::shutdown() {
  if (harness_ && subscription_id_ >= 0) {
    harness_->unsubscribe(subscription_id_);
    subscription_id_ = -1;
  }
}

void TuiState::drainEvents() {
  for (const auto &ev : event_queue_.drainAll()) {
    onEvent(ev);
  }
}

void TuiState::onEvent(const shared::AppEvent &ev) {
  std::visit(
      [&](auto &&e) {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, AgentThinking>) {
          stream_state_.handleAgentThinking(e);
        } else if constexpr (std::is_same_v<T, AgentText>) {
          stream_state_.handleAgentText(e);
        } else if constexpr (std::is_same_v<T, AgentTurnCompleted>) {
          stream_state_.handleAgentTurnCompleted(e);
        } else if constexpr (std::is_same_v<T, AgentProviderWaiting>) {
          stream_state_.handleAgentProviderWaiting(e);
        } else if constexpr (std::is_same_v<T, AgentToolCallChunk>) {
          stream_state_.handleAgentToolCallChunk(e);
        } else if constexpr (std::is_same_v<T, AgentToolCall>) {
          stream_state_.handleAgentToolCall(e);
        } else if constexpr (std::is_same_v<T, ThreadChanged>) {
          thread_ = e.metadata;
          focused_agent_id_ = harness_->focusedAgentId();
          history_ = harness_->getAgentHistoryPtr(focused_agent_id_);
          pending_modal_clear_ = true;

          if (title_model_) {
            title_model_->title = thread_.title;
            title_model_->thread_id = thread_.threadId;
          }
          setViewMode(ViewMode::Chat);

          if (chat_component_) {
            chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
          }
        } else if constexpr (std::is_same_v<T, ThreadLocked>) {
          auto locked_modal =
              ThreadLockedModal::create(*this, e.threadId, e.ownerPid);
          openModalDirect(locked_modal);
        } else if constexpr (std::is_same_v<T, AgentCompactionThinking>) {
          stream_state_.handleAgentCompactionThinking(e);
        } else if constexpr (std::is_same_v<T, AgentCompactionText>) {
          stream_state_.handleAgentCompactionText(e);
        } else if constexpr (std::is_same_v<T, ContextCompacted>) {
          stream_state_.handleContextCompacted(e);
        } else if constexpr (std::is_same_v<T, AgentProcessSpawned>) {
          stream_state_.handleAgentProcessSpawned(e);
        } else if constexpr (std::is_same_v<T, AgentProcessOutput>) {
          stream_state_.handleAgentProcessOutput(e);
        } else if constexpr (std::is_same_v<T, AgentSpawned>) {
          if (e.parentId.empty() && focused_agent_id_.empty()) {
            focused_agent_id_ = e.agentId;
            history_ = harness_->getAgentHistoryPtr(focused_agent_id_);
            if (chat_component_) {
              chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
            }
          }
          stream_state_.handleAgentSpawned(e, focused_agent_id_);
        } else if constexpr (std::is_same_v<T, UserMessageSent>) {
          if (focused_agent_id_.empty()) {
            focused_agent_id_ = harness_->focusedAgentId();
            history_ = harness_->getAgentHistoryPtr(focused_agent_id_);
          }
          if (chat_component_) {
            chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
          }
        } else if constexpr (std::is_same_v<T, HistoryUndone>) {
          if (chat_component_) {
            chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
          }
        } else if constexpr (std::is_same_v<T, AgentError>) {
          stream_state_.handleAgentError(e);
        } else if constexpr (std::is_same_v<T, AgentRetrying>) {
          stream_state_.handleAgentRetrying(e);
        } else if constexpr (std::is_same_v<T, AgentRetryFailed>) {
          stream_state_.handleAgentRetryFailed(e);
        }
      },
      ev);

  if (screen_) {
    screen_->PostEvent(ftxui::Event::Custom);
  }
}

std::string TuiState::statusText() const { return "unknown"; }

void TuiState::updateStatusModel() {
  if (!status_model_)
    return;
  if (!focused_agent_id_.empty()) {
    auto agent =
        firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    if (agent) {
      const auto &ctx = agent->getContext();
      status_model_->status_text = statusToString(ctx.state.currentStatus);
      status_model_->model_name =
          ctx.config.providerId + "/" + ctx.config.modelId;
      status_model_->purpose = ctx.identity.role;
      return;
    }
  }
  status_model_->status_text = "idle";
  status_model_->model_name = "";
  status_model_->purpose = "";
}

ftxui::Component TuiState::root() {
  if (root_component_)
    return root_component_;

  auto title_bar = TitleBar(title_model_);
  auto status_bar = StatusBar(status_model_);

  auto input_bar = InputBar(input_model_, [this](const std::string &text) {
    if (!text.empty() && text[0] == '/') {
      CommandCtx ctx{this};
      if (firmius::tui::CommandManager::instance().executeCommand(ctx, text)) {
        return; // Command handled successfully
      }
    }

    if (view_mode_ == ViewMode::Welcome) {
      // If we are on the welcome screen, typing a message automatically starts
      // a thread
      if (harness_) {
        // Auto-create thread in current directory with default lead persona
        std::string cwd = std::filesystem::current_path().string();
        harness_->newThread({}, cwd, "firmius");
        harness_->send(text);
      }
      setViewMode(ViewMode::Chat);
    } else {
      if (harness_) {
        harness_->send(text);
      }
    }
  });
  auto chat = ChatWindow(
      [this]() -> const firmius::shared::AgentHistory * {
        if (focused_agent_id_.empty())
          return nullptr;
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (!agent)
          return history_.get();
        return agent->getContext().history.get();
      },
      [this]() {
        std::vector<ftxui::Element> live_rows;
        const auto *s = stream_state_.getStream(focused_agent_id_);

        auto decorateMsg = [](const ftxui::Element &content) {
          return ftxui::hbox({ftxui::text("* ") | ftxui::bold |
                                  ftxui::color(ftxui::Color::Yellow),
                              content | ftxui::flex}) |
                 ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 120);
        };

        if (s) {
          if (!s->thinking.empty()) {
            live_rows.push_back(
                decorateMsg(ftxui::vbox({ftxui::text("[thinking]") | ftxui::dim,
                                         RenderMarkdown(s->thinking, true)})));
          }
          if (!s->text.empty()) {
            live_rows.push_back(decorateMsg(RenderMarkdown(s->text)));
          }
          if (!s->compaction_thinking.empty()) {
            live_rows.push_back(decorateMsg(
                ftxui::vbox({ftxui::text("[compacting:thinking]") | ftxui::dim,
                             RenderMarkdown(s->compaction_thinking, true)})));
          }
          if (!s->compaction_text.empty()) {
            live_rows.push_back(decorateMsg(
                ftxui::vbox({ftxui::text("[compacting]") | ftxui::dim,
                             RenderMarkdown(s->compaction_text, true)})));
          }
          if (s->provider_waiting) {
            live_rows.push_back(
                decorateMsg(ftxui::text("[provider waiting]") | ftxui::dim));
          }
        }

        const auto &tool_order = stream_state_.getToolOrder();
        const auto &tool_calls = stream_state_.getToolCalls();
        for (const auto &id : tool_order) {
          auto it_tool = tool_calls.find(id);
          if (it_tool == tool_calls.end())
            continue;
          const auto &view = it_tool->second;
          if (!view || view->agentId != focused_agent_id_)
            continue;
          live_rows.push_back(decorateMsg(ToolBlock(view)->Render()));
        }

        // Persistent error messages (rendered in red)
        for (const auto &err : stream_state_.getErrorMessages()) {
          live_rows.push_back(ftxui::text(err) | ftxui::bold |
                              ftxui::color(ftxui::Color::Red));
        }

        // Ephemeral retry status (rendered dim yellow, disappears when
        // resolved)
        const auto &retry = stream_state_.getRetryStatus();
        if (!retry.empty()) {
          live_rows.push_back(ftxui::text(retry) | ftxui::dim |
                              ftxui::color(ftxui::Color::Yellow));
        }

        return live_rows;
      });
  chat_component_ = chat;

  auto container = ftxui::Container::Vertical({
      input_bar,
      chat,
  });

  auto welcome_screen = ftxui::Renderer([] {
    return ftxui::vbox({
               ftxui::text("Welcome to Firmius") | ftxui::bold | ftxui::center,
               ftxui::text("Type a message to start a new thread, or use "
                           "/threads to resume.") |
                   ftxui::dim | ftxui::center,
           }) |
           ftxui::flex | ftxui::center;
  });

  auto base_view =
      ftxui::Renderer(container, [this, title_bar, status_bar, input_bar, chat,
                                  welcome_screen] {
        drainEvents();
        // Deferred modal clearing: drain here where it's safe
        if (pending_modal_clear_) {
          modals_.clear();
          pending_modal_clear_ = false;
        }

        updateStatusModel();

        if (modals_.empty()) {
          input_bar->TakeFocus();
        }

        auto chat_area = (view_mode_ == ViewMode::Chat)
                             ? (chat->Render() | ftxui::flex)
                             : welcome_screen->Render();

        auto bottom_bar = ftxui::vbox({
            ftxui::separatorLight(),
            input_bar->Render(),
            ftxui::separatorLight(),
            status_bar->Render(),
        });

        if (view_mode_ == ViewMode::Welcome) {
          // Hide title bar context in welcome
          return ftxui::vbox({
                     chat_area,
                     bottom_bar,
                 }) |
                 ftxui::flex;
        }

        return ftxui::vbox({
                   title_bar->Render(),
                   chat_area,
                   bottom_bar,
               }) |
               ftxui::flex;
      });

  // Layer modals using dbox
  auto modal_renderer = ftxui::Renderer(base_view, [this, base_view]() {
    ftxui::Element current = base_view->Render();
    for (const auto &modal : modals_) {
      current = ftxui::dbox(
          {current, modal->Render() | ftxui::clear_under | ftxui::center});
    }
    return current;
  });

  root_component_ = ftxui::CatchEvent(modal_renderer, [this, chat](
                                                          ftxui::Event event) {
    if (!modals_.empty()) {
      // Forward event to the top modal
      bool handled = modals_.back()->OnEvent(event);
      if (handled)
        return true;

      // Block background interaction if a modal is up
      if (event.is_mouse() || event.is_character()) {
        return true;
      }
    }

    if (event.is_mouse()) {
      auto &m = event.mouse();
      if (m.button == ftxui::Mouse::WheelUp ||
          m.button == ftxui::Mouse::WheelDown) {
        if (chat_component_) {
          return chat_component_->OnEvent(event);
        }
      }
    }
    if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown ||
        event == ftxui::Event::Home || event == ftxui::Event::End) {
      if (chat_component_) {
        return chat_component_->OnEvent(event);
      }
    }
    if (event == ftxui::Event::Escape) {
      if (!modals_.empty()) {
        popModal();
        return true;
      }
      if (harness_)
        harness_->abort();
      return true;
    }
    if (event == ftxui::Event::Special("\x10")) { // Ctrl+P (Parent)
      if (harness_) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (agent && !agent->getContext().identity.parentId.empty()) {
          std::string parentId = agent->getContext().identity.parentId;
          if (harness_->setFocusedAgent(parentId)) {
            focused_agent_id_ = parentId;
            if (history_) {
              *history_ = harness_->getAgentHistory(focused_agent_id_);
            } else {
              history_ = harness_->getAgentHistoryPtr(focused_agent_id_);
            }
            if (chat_component_)
              chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
          }
        }
      }
      return true;
    }

    if (event == ftxui::Event::Special("\x0E") ||
        event ==
            ftxui::Event::Special("\x02")) { // Ctrl+N (Next), Ctrl+B (Prev)
      if (harness_) {
        auto agents = harness_->listAgents();
        if (!agents.empty()) {
          auto it = std::find(agents.begin(), agents.end(), focused_agent_id_);
          if (it != agents.end()) {
            if (event == ftxui::Event::Special("\x0E")) { // Next
              ++it;
              if (it == agents.end())
                it = agents.begin();
            } else { // Prev
              if (it == agents.begin())
                it = agents.end();
              --it;
            }
            if (harness_->setFocusedAgent(*it)) {
              focused_agent_id_ = *it;
              if (history_) {
                *history_ = harness_->getAgentHistory(focused_agent_id_);
              } else {
                history_ = harness_->getAgentHistoryPtr(focused_agent_id_);
              }
              if (chat_component_)
                chat_component_->OnEvent(
                    ftxui::Event::Special("ThreadChanged"));
            }
          }
        }
      }
      return true;
    }

    return false;
  });

  return root_component_;
}

} // namespace firmius::tui
