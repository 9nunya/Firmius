#include "ActivePlanState.hpp"
#include "TUIState.hpp"
#include "AgentRegistry.hpp"
#include "NotificationManager.hpp"
#include "ThemeManager.hpp"
#include "TUIHotkeys.hpp"
#include "UIState.hpp"
#include "WorkPanelLayout.hpp"
#include "agents/PurposeLoader.hpp"
#include "commands/CommandManager.hpp"
#include "components/AgentStrip.hpp"
#include "components/ChatWindow.hpp"
#include "components/GlintEffect.hpp"
#include "components/HelpOverlay.hpp"
#include "components/InputBar.hpp"
#include "components/Markdown.hpp"
#include "components/PlanLane.hpp"
#include "components/TodoLane.hpp"
#include "components/StatusBar.hpp"
#include "components/TitleBar.hpp"
#include "components/TranscriptGrouping.hpp"
#include "components/ToolBlock.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalRegistry.hpp"
#include "modals/PermissionPromptModal.hpp"
#include "modals/ThreadLockedModal.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

static std::string resolveDefaultLeadPersona(
    const firmius::core::Harness *harness) {
  std::string fallback = "lead";
  if (!harness)
    return fallback;
  const auto &cfg = harness->getConfig();
  if (!cfg.defaultLeadPersona.empty())
    return cfg.defaultLeadPersona;
  return fallback;
}

static std::string resolvePersonaTitle(const std::string &personaName) {
  try {
    return firmius::core::PurposeLoader::load(personaName).title;
  } catch (...) {
    return personaName;
  }
}

static std::string firmiusThreadsPath() {
  return std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") +
         "/.firmius/threads";
}

static std::vector<std::string> getSwitchableLeadPersonas() {
  auto purposes = firmius::core::PurposeLoader::listSwitchablePurposes();
  if (purposes.empty()) {
    purposes.push_back("lead");
  }
  return purposes;
}

static std::string permissionModeToDisplayName(
    shared::ThreadPermissionMode mode) {
  switch (mode) {
  case shared::ThreadPermissionMode::Request:
    return "Request";
  case shared::ThreadPermissionMode::AlwaysAllow:
    return "Always Allow";
  case shared::ThreadPermissionMode::DenyAll:
    return "Deny All";
  }
  return "Request";
}

static std::string permissionResponseToDisplayName(
    shared::PermissionResponse response) {
  switch (response) {
  case shared::PermissionResponse::AllowOnce:
    return "Allow Once";
  case shared::PermissionResponse::AllowAlways:
    return "Allow Always";
  case shared::PermissionResponse::Deny:
    return "Deny";
  }
  return "Deny";
}

class ScreenShaderNode : public ftxui::Node {
public:
  explicit ScreenShaderNode(ftxui::Element child)
      : ftxui::Node({std::move(child)}) {}

  void ComputeRequirement() override {
    requirement_ = children_[0]->requirement();
  }

  void SetBox(ftxui::Box box) override {
    box_ = box;
    children_[0]->SetBox(box);
  }

  void Render(ftxui::Screen &screen) override { children_[0]->Render(screen); }
};

class DarkenNode : public ScreenShaderNode {
public:
  DarkenNode(ftxui::Element child, uint8_t alpha)
      : ScreenShaderNode(std::move(child)),
        overlay_(ftxui::Color::RGBA(0, 0, 0, alpha)) {}

  void Render(ftxui::Screen &screen) override {
    ScreenShaderNode::Render(screen);
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        auto &pixel = screen.PixelAt(x, y);
        if (pixel.foreground_color != ftxui::Color::Default) {
          pixel.foreground_color =
              ftxui::Color::Blend(pixel.foreground_color, overlay_);
        }
        if (pixel.background_color != ftxui::Color::Default) {
          pixel.background_color =
              ftxui::Color::Blend(pixel.background_color, overlay_);
        }
      }
    }
  }

private:
  ftxui::Color overlay_;
};

static ftxui::Element DarkenElement(ftxui::Element child, uint8_t alpha = 96) {
  return std::make_shared<DarkenNode>(std::move(child), alpha);
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
  if (modal) {
    modal->TakeFocus();
  }
}

void TuiState::popModal() { postEvent(ftxui::Event::Special("PopModal")); }

void TuiState::popModalImmediate() {
  if (!modals_.empty()) {
    modals_.pop_back();
  }
  if (modals_.empty()) {
    if (input_component_) {
      input_component_->TakeFocus();
    }
    // Also explicitly post an event to ensure the screen re-renders and focus
    // is acknowledged
    postEvent(ftxui::Event::Custom);
  } else {
    modals_.back()->TakeFocus();
  }
}

void TuiState::replaceModalDirect(ftxui::Component modal) {
  if (!modals_.empty()) {
    modals_.pop_back();
  }
  modals_.push_back(modal);
  if (modal) {
    modal->TakeFocus();
  }
}

void TuiState::clearModals() { modals_.clear(); }

void TuiState::deferUiMutation(std::function<void()> action) {
  if (!action) {
    return;
  }
  deferred_ui_mutations_.push_back(std::move(action));
  postEvent(ftxui::Event::Custom);
}

void TuiState::postEvent(ftxui::Event event) {
  if (screen_) {
    screen_->PostEvent(event);
  }
}

bool TuiState::cycleThreadPermissionMode() {
  if (!harness_ || thread_.threadId.empty()) {
    NotificationManager::instance().notifyWarning(
        "Permissions", "No active thread to update.",
        std::chrono::milliseconds(1600));
    return false;
  }

  harness_->cycleCurrentThreadPermissionMode();
  return true;
}

bool TuiState::hasActiveThread() const { return !thread_.threadId.empty(); }

std::string TuiState::currentThreadId() const { return thread_.threadId; }

shared::ThreadPermissionMode TuiState::currentThreadPermissionMode() const {
  return thread_.permissionMode;
}

void TuiState::init(firmius::core::Harness &harness,
                    const shared::ThreadMetadata &thread,
                    const std::string &focused_agent_id) {
  harness_ = &harness;
  thread_ = thread;
  focused_agent_id_ = focused_agent_id;

  if (!focused_agent_id_.empty()) {
    history_ = harness_->getAgentHistoryPtr(focused_agent_id_);
    if (!history_) {
      // Agent is not active or hasn't spawned in the current harness lifecycle.
      // Load history from persistence as a fallback for viewing.
      auto fallback_hist =
          firmius::core::ThreadManager(
              std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") +
              "/.firmius/threads")
              .loadAgentHistory(thread_.threadId, focused_agent_id_);
      history_ = std::make_shared<firmius::shared::AgentHistory>(fallback_hist);
    }
  } else {
    history_ = nullptr;
  }

  title_model_ = std::make_shared<TitleBarModel>();
  title_model_->title = thread_.title;
  title_model_->thread_id = thread_.threadId;

  status_model_ = std::make_shared<StatusBarModel>();
  status_model_->status_text = "idle";

  input_model_ = std::make_shared<InputBarModel>();
  input_model_->buffer = &input_;
  input_model_->cursor = &cursor_;
  input_model_->placeholder = "Type a message...";

  // Set up vision capability check
  input_model_->check_vision_capable = [this]() -> bool {
    if (!harness_ || focused_agent_id_.empty())
      return false;
    auto agent = firmius::core::AgentRegistry::instance().getAgent(
        focused_agent_id_);
    if (!agent)
      return false;
    auto &ctx =
        const_cast<firmius::shared::AgentContext &>(agent->getContext());
    auto provider =
        firmius::provider::ProviderRegistry::instance().getProvider(
            ctx.config.providerId);
    if (!provider)
      return false;
    auto info = provider->getModelInfo(ctx.config.modelId);
    return std::find(info.modalities.begin(), info.modalities.end(),
                     "image") != info.modalities.end();
  };

  // Set up notification function
  input_model_->show_notification = [](const std::string &title,
                                       const std::string &message) {
    NotificationManager::instance().notifyError(title, message, false);
  };

  agent_strip_model_ = std::make_shared<AgentStripModel>();
  agent_strip_model_->on_item_click = [this](const std::string& agent_id) {
    if (harness_ && harness_->setFocusedAgent(agent_id)) {
      focused_agent_id_ = agent_id;
      if (history_) {
        *history_ = harness_->getAgentHistory(agent_id);
      } else {
        history_ = harness_->getAgentHistoryPtr(agent_id);
      }
      if (chat_component_) {
        chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
      }
      updateAgentStripModel();
      updateStatusModel();
      updateTodoLaneModel();
    }
  };

  plan_lane_model_ = std::make_shared<PlanLaneModel>();
  todo_lane_model_ = std::make_shared<TodoLaneModel>();
  active_plan_state_.setExpanded(plan_lane_expanded_);
  active_plan_state_.hydrateForThread(thread_, loadActivePlanForThread(thread_));
  updatePlanLaneModel();
  updateTodoLaneModel();

  subscription_id_ =
      harness_->subscribe([this](const firmius::shared::AppEvent &ev) {
        event_queue_.push(ev);
        if (screen_) {
          screen_->PostEvent(ftxui::Event::Custom);
        }
      });

  updateStatusModel();
  updateAgentStripModel();
  updatePlanLaneModel();
  updateTodoLaneModel();
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

        updateStatusModel();
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

          if (focused_agent_id_.empty()) {
            auto agents = harness_->listAgents(thread_.threadId);
            if (!agents.empty()) {
              focused_agent_id_ = agents.front();
            }
          }

          if (!focused_agent_id_.empty()) {
            history_ = harness_->getAgentHistoryPtr(focused_agent_id_);
            if (!history_) {
              auto fallback_hist =
                  firmius::core::ThreadManager(
                      std::string(std::getenv("HOME") ? std::getenv("HOME")
                                                      : "/tmp") +
                      "/.firmius/threads")
                      .loadAgentHistory(thread_.threadId, focused_agent_id_);
              history_ = std::make_shared<firmius::shared::AgentHistory>(
                  fallback_hist);
            }
          } else {
            history_ = nullptr;
          }

          pending_modal_clear_ = true;
          stream_state_.handleThreadChanged();

          // Rebuild tool calls from history for ALL agents in the thread
          // This ensures parent agents' summon_subagent tool calls get their
          // subagent_tool_log populated from subagent histories
          // Process agents in order (lead agent first, then subagents)
          auto all_agents = harness_->listAgents(thread_.threadId);

          // First pass: create all tool calls for all agents
          for (const auto &agent_id : all_agents) {
            auto agent_hist = harness_->getAgentHistoryPtr(agent_id);
            std::shared_ptr<firmius::shared::AgentHistory> owned_hist;
            if (!agent_hist) {
              // Try loading from disk
              auto fallback_hist =
                  firmius::core::ThreadManager(
                      std::string(std::getenv("HOME") ? std::getenv("HOME")
                                                      : "/tmp") +
                      "/.firmius/threads")
                      .loadAgentHistory(thread_.threadId, agent_id);
              if (!fallback_hist.turns.empty()) {
                owned_hist = std::make_shared<firmius::shared::AgentHistory>(
                    std::move(fallback_hist));
                agent_hist = owned_hist;
              }
            }
            if (agent_hist) {
              // Pass false for populate_subagent_log - just create tool calls
              stream_state_.rebuildToolCallsFromHistory(
                  agent_id, agent_hist.get(), thread_.threadId, false);
            }
          }

          // Second pass: populate subagent_tool_log for all summon_subagent
          // calls
          for (const auto &agent_id : all_agents) {
            auto agent_hist = harness_->getAgentHistoryPtr(agent_id);
            std::shared_ptr<firmius::shared::AgentHistory> owned_hist;
            if (!agent_hist) {
              auto fallback_hist =
                  firmius::core::ThreadManager(
                      std::string(std::getenv("HOME") ? std::getenv("HOME")
                                                      : "/tmp") +
                      "/.firmius/threads")
                      .loadAgentHistory(thread_.threadId, agent_id);
              if (!fallback_hist.turns.empty()) {
                owned_hist = std::make_shared<firmius::shared::AgentHistory>(
                    std::move(fallback_hist));
                agent_hist = owned_hist;
              }
            }
            if (agent_hist) {
              // Pass true for populate_subagent_log - populate
              // subagent_tool_log
              stream_state_.rebuildToolCallsFromHistory(
                  agent_id, agent_hist.get(), thread_.threadId, true);
            }
          }

          if (title_model_) {
            title_model_->title = thread_.title;
            title_model_->thread_id = thread_.threadId;
          }
          active_plan_state_.setExpanded(plan_lane_expanded_);
          active_plan_state_.hydrateForThread(thread_,
                                              loadActivePlanForThread(thread_));
          updatePlanLaneModel();
          updateTodoLaneModel();
          setViewMode(ViewMode::Chat);

          updateStatusModel();
          updateAgentStripModel();

          if (chat_component_) {
            chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
          }
        } else if constexpr (std::is_same_v<T, ThreadMetadataUpdated>) {
          if (e.threadId == thread_.threadId) {
            const std::string previous_active_plan_id = thread_.activePlanId;
            auto previousMode = thread_.permissionMode;
            thread_ = e.metadata;
            if (title_model_) {
              title_model_->title = thread_.title;
              title_model_->thread_id = thread_.threadId;
            }
            if (previous_active_plan_id != thread_.activePlanId) {
              active_plan_state_.setExpanded(plan_lane_expanded_);
              active_plan_state_.hydrateForThread(
                  thread_, loadActivePlanForThread(thread_));
              updatePlanLaneModel();
              updateTodoLaneModel();
            }
            if (previousMode != thread_.permissionMode) {
              NotificationManager::instance().notifyInfo(
                  "Permissions",
                  "Thread mode: " +
                      permissionModeToDisplayName(thread_.permissionMode),
                  std::chrono::milliseconds(1500));
            }
          }
        } else if constexpr (std::is_same_v<T, PermissionEscalationRequest>) {
          auto modal = std::make_shared<PermissionPromptModal>(
              e, [this, requestId = e.requestId](PermissionResponse response) {
                if (harness_) {
                  harness_->resolvePermissionEscalation(requestId, response);
                }
              });
          openModalDirect(modal->create(*this));
        } else if constexpr (std::is_same_v<T, PermissionEscalationResolved>) {
          NotificationManager::instance().notifyInfo(
              "Permission",
              permissionResponseToDisplayName(e.response),
              std::chrono::milliseconds(1200));
        } else if constexpr (std::is_same_v<T, ThreadLocked>) {
          auto locked_modal =
              ThreadLockedModal::create(*this, e.threadId, e.ownerPid);
          openModalDirect(locked_modal);
        } else if constexpr (std::is_same_v<T, AgentCompacting>) {
          stream_state_.handleAgentCompacting(e);
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
        } else if constexpr (std::is_same_v<T, AgentAccountSwitched>) {
          stream_state_.handleAgentAccountSwitched(e);
          NotificationManager::instance().notifyInfo(
              "Account Switch", "Switched to " + e.accountLocator,
              std::chrono::milliseconds(4000));
        } else if constexpr (std::is_same_v<T, AgentError>) {
          if (e.agentId.empty() && !e.message.empty()) {
            NotificationManager::instance().notifyWarning(
                "Thread Recovery", e.message, std::chrono::milliseconds(5000));
          }
        } else if constexpr (std::is_same_v<T, AgentRetrying>) {
          stream_state_.handleAgentRetrying(e);
          std::string retryMsg = "Attempt " + std::to_string(e.attempt) + "/" +
                                 std::to_string(e.maxAttempts);
          if (!e.reason.empty())
            retryMsg += " - " + e.reason;
          if (e.delayMs > 0)
            retryMsg += " (~" + std::to_string(e.delayMs / 1000) + "s)";
          NotificationManager::instance().notifyWarning(
              "Retrying", retryMsg,
              std::chrono::milliseconds(std::max(e.delayMs, 3000)));
        } else if constexpr (std::is_same_v<T, AgentRetryFailed>) {
          stream_state_.handleAgentRetryFailed(e);
          NotificationManager::instance().notifyError(
              "Retry Failed",
              e.reason.empty() ? "All retry attempts exhausted" : e.reason,
              false);
        } else if constexpr (std::is_same_v<T, AgentCompleted>) {
          stream_state_.handleAgentCompleted(e);
        } else if constexpr (std::is_same_v<T, AgentInterrupted>) {
          stream_state_.handleAgentInterrupted(e);
        } else if constexpr (std::is_same_v<T, ThreadTitleUpdated>) {
          if (title_model_) {
            title_model_->title = e.title;
          }
          thread_.title = e.title;
        } else if constexpr (std::is_same_v<T, MessageQueued>) {
          stream_state_.handleMessageQueued(e);
        } else if constexpr (std::is_same_v<T, MessageDequeued>) {
          stream_state_.handleMessageDequeued(e);
        }
      },
      ev);

  if (active_plan_state_.handleEvent(ev, thread_.threadId)) {
    if (const auto &plan = active_plan_state_.activePlan(); plan.has_value()) {
      thread_.activePlanId = plan->id;
    }
    updatePlanLaneModel();
    updateTodoLaneModel();
  }

  updateStatusModel();
  updateAgentStripModel();
  updateTodoLaneModel();

  if (screen_) {
    screen_->PostEvent(ftxui::Event::Custom);
  }
}

std::string TuiState::statusText() const { return "unknown"; }

void TuiState::updateStatusModel() {
  if (!status_model_)
    return;
  status_model_->permission_mode = thread_.permissionMode;
  status_model_->live_processes = 0;
  status_model_->background_processes = 0;
  if (!focused_agent_id_.empty()) {
    auto agent =
        firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    if (agent) {
      const auto &ctx = agent->getContext();
      auto process_counts = stream_state_.getProcessCounts(focused_agent_id_);
      status_model_->status_text = statusToString(ctx.state.currentStatus);
      status_model_->model_name =
          ctx.config.providerId + "/" + ctx.config.modelId;
      status_model_->model_variant = ctx.config.modelVariant;
      status_model_->purpose = ctx.identity.role;
      status_model_->agent_name = ctx.identity.friendlyName.empty()
                                      ? ctx.identity.name
                                      : ctx.identity.friendlyName;
      status_model_->context_used = ctx.aggregateMetrics.tokens.contextSize;
      auto provider =
          firmius::provider::ProviderRegistry::instance().getProvider(
              ctx.config.providerId);
      if (provider) {
        auto info = provider->getModelInfo(ctx.config.modelId);
        status_model_->context_max = info.contextWindow;
      }
      status_model_->is_active =
          ctx.state.currentStatus == AgentStatus::Streaming ||
          ctx.state.currentStatus == AgentStatus::ExecutingTool ||
          ctx.state.currentStatus == AgentStatus::ProviderWaiting;
      status_model_->live_processes = process_counts.live;
      status_model_->background_processes = process_counts.background;
      return;
    }
  }
  status_model_->status_text = "idle";
  std::string personaName = resolveDefaultLeadPersona(harness_);
  std::string personaTitle = resolvePersonaTitle(personaName);
  if (harness_) {
    const auto &cfg = harness_->getConfig();
    status_model_->model_name = cfg.defaultProviderId + "/" + cfg.defaultModelId;
    status_model_->model_variant = cfg.defaultModelVariant;
  } else {
    status_model_->model_name = "";
    status_model_->model_variant.clear();
  }
  status_model_->purpose = personaTitle;
  status_model_->agent_name = personaTitle;
  status_model_->context_used = 0;
  status_model_->context_max = 0;
  status_model_->is_active = false;
  status_model_->live_processes = 0;
  status_model_->background_processes = 0;
}

void TuiState::updateAgentStripModel() {
  if (!agent_strip_model_)
    return;
  agent_strip_model_->items.clear();
  if (focused_agent_id_.empty())
    return;

  auto countHistoryToolCalls =
      [](const firmius::shared::AgentHistory *history) {
        if (!history)
          return 0;
        int count = 0;
        for (const auto &turn : history->turns) {
          for (const auto &msg : turn.messages) {
            for (const auto &part : msg.content) {
              if (std::holds_alternative<firmius::shared::ToolCallContent>(
                      part))
                ++count;
            }
          }
        }
        return count;
      };

  std::unordered_map<std::string, int> live_tool_call_counts;
  const auto &tool_calls = stream_state_.getToolCalls();
  for (const auto &[_, view] : tool_calls) {
    if (!view || view->agentId.empty())
      continue;
    ++live_tool_call_counts[view->agentId];
  }

  auto all_ids = firmius::core::AgentRegistry::instance().listAll();
  std::string parent_focus = focused_agent_id_;
  std::string focused_parent;
  auto focused_agent =
      firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
  if (focused_agent) {
    focused_parent = focused_agent->getContext().identity.parentId;
  }
  bool has_children = false;
  for (const auto &id : all_ids) {
    auto child = firmius::core::AgentRegistry::instance().getAgent(id);
    if (!child)
      continue;
    if (child->getContext().identity.parentId == focused_agent_id_) {
      has_children = true;
      break;
    }
  }
  if (!has_children && !focused_parent.empty()) {
    parent_focus = focused_parent;
  }

  std::vector<AgentStripItem> all_items;
  all_items.reserve(all_ids.size());
  size_t focused_index = 0;
  bool focus_found = false;
  size_t candidate_index = 0;
  uint64_t now_ms =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
  for (const auto &id : all_ids) {
    auto child = firmius::core::AgentRegistry::instance().getAgent(id);
    if (!child)
      continue;
    const auto &ctx = child->getContext();
    if (ctx.identity.parentId != parent_focus)
      continue;
    AgentStripItem item;
    item.id = id;
    std::string display_title = ctx.identity.role;
    if (display_title.empty())
      display_title = ctx.identity.name;
    std::string stream_title = stream_state_.getAgentTitle(id);
    if (!stream_title.empty())
      display_title = stream_title;
    item.title = display_title;
    item.purpose = resolvePersonaTitle(ctx.config.personaName);
    item.model_name = ctx.config.modelId; // Use modelId directly,
                                          // PrettifyModelName handles prefixes
    item.model_variant = ctx.config.modelVariant;
    item.status_text = statusToString(ctx.state.currentStatus);
    item.is_busy = ctx.state.currentStatus == AgentStatus::Streaming ||
                   ctx.state.currentStatus == AgentStatus::ExecutingTool ||
                   ctx.state.currentStatus == AgentStatus::ProviderWaiting;
    if (item.is_busy) {
      auto it = agent_work_start_ms_.find(id);
      if (it == agent_work_start_ms_.end()) {
        auto inserted = agent_work_start_ms_.emplace(id, now_ms);
        it = inserted.first;
      }
      item.working_since_ms = it->second;
    } else {
      agent_work_start_ms_.erase(id);
    }
    auto provider = firmius::provider::ProviderRegistry::instance().getProvider(
        ctx.config.providerId);
    if (provider) {
      auto info = provider->getModelInfo(ctx.config.modelId);
      if (info.contextWindow > 0) {
        item.context_percent =
            static_cast<float>(ctx.aggregateMetrics.tokens.contextSize) /
            info.contextWindow;
      }
    }
    int history_tool_calls =
        countHistoryToolCalls(child->getContext().history.get());
    auto live_it = live_tool_call_counts.find(id);
    int live_tool_calls =
        (live_it != live_tool_call_counts.end()) ? live_it->second : 0;
    item.tool_call_count = history_tool_calls + live_tool_calls;
    item.is_focused = focused_agent_id_ == id;
    
    // Calculate hierarchy depth
    item.parent_id = ctx.identity.parentId;
    item.hierarchy_depth = 0;
    std::string current_parent = ctx.identity.parentId;
    while (!current_parent.empty()) {
      item.hierarchy_depth++;
      auto parent = firmius::core::AgentRegistry::instance().getAgent(current_parent);
      if (parent) {
        current_parent = parent->getContext().identity.parentId;
      } else {
        break;
      }
    }
    
    // Check if this agent has children
    item.has_children = false;
    for (const auto &other_id : all_ids) {
      if (other_id == id) continue;
      auto other = firmius::core::AgentRegistry::instance().getAgent(other_id);
      if (other && other->getContext().identity.parentId == id) {
        item.has_children = true;
        break;
      }
    }
    
    if (item.is_focused) {
      focused_index = candidate_index;
      focus_found = true;
    }
    all_items.push_back(std::move(item));
    ++candidate_index;
  }

  const size_t total_items = all_items.size();
  if (total_items == 0) {
    agent_strip_model_->view_offset = 0;
    return;
  }

  size_t visible_rows = std::min(kAgentStripVisibleRows, total_items);
  size_t offset = agent_strip_model_->view_offset;
  if (offset > total_items - visible_rows) {
    offset = total_items - visible_rows;
  }

  if (focus_found) {
    if (focused_index < offset) {
      offset = focused_index;
    } else if (focused_index >= offset + visible_rows) {
      offset = focused_index - visible_rows + 1;
    }
  }

  agent_strip_model_->view_offset = offset;
  for (size_t i = 0; i < visible_rows; ++i) {
    agent_strip_model_->items.push_back(std::move(all_items[offset + i]));
  }
}

void TuiState::updatePlanLaneModel() {
  if (!plan_lane_model_) {
    return;
  }
  *plan_lane_model_ = active_plan_state_.model();
}

std::string TuiState::findExecutorChunkTitle(
    const std::optional<shared::Plan> &plan) const {
  if (!plan.has_value() || focused_agent_id_.empty()) {
    return "";
  }
  for (const auto &chunk : plan->chunks) {
    if (chunk.assignedAgentId != focused_agent_id_) {
      continue;
    }
    if (chunk.status == shared::WorkChunkStatus::Done ||
        chunk.status == shared::WorkChunkStatus::Cancelled) {
      continue;
    }
    return chunk.title;
  }
  return "";
}

void TuiState::updateTodoLaneModel() {
  if (!todo_lane_model_) {
    return;
  }

  todo_lane_model_->visible = false;
  todo_lane_model_->owner_label.clear();
  todo_lane_model_->show_chunk_header = false;
  todo_lane_model_->chunk_title.clear();
  todo_lane_model_->rows.clear();

  if (thread_.threadId.empty() || focused_agent_id_.empty()) {
    return;
  }

  try {
    firmius::core::ThreadManager tm(firmiusThreadsPath());
    const auto todo = tm.getAgentTodo(thread_.threadId, focused_agent_id_);
    if (todo.items.empty()) {
      return;
    }

    todo_lane_model_->visible = true;
    todo_lane_model_->owner_label = focused_agent_id_.substr(0, 8);
    auto focusedAgent =
        firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    if (focusedAgent) {
      const auto &ctx = focusedAgent->getContext();
      if (!ctx.identity.friendlyName.empty()) {
        todo_lane_model_->owner_label = ctx.identity.friendlyName;
      }
      if (ctx.config.personaName == "executor") {
        const auto activePlan = loadActivePlanForThread(thread_);
        const std::string chunkTitle = findExecutorChunkTitle(activePlan);
        if (!chunkTitle.empty()) {
          todo_lane_model_->show_chunk_header = true;
          todo_lane_model_->chunk_title = chunkTitle;
        }
      }
    }

    todo_lane_model_->rows.reserve(todo.items.size());
    for (const auto &item : todo.items) {
      todo_lane_model_->rows.push_back({item.id, item.text, item.status});
    }
    std::sort(todo_lane_model_->rows.begin(), todo_lane_model_->rows.end(),
              [](const TodoLaneRow &lhs, const TodoLaneRow &rhs) {
                return lhs.id < rhs.id;
              });
  } catch (...) {
  }
}

std::optional<shared::Plan>
TuiState::loadActivePlanForThread(const shared::ThreadMetadata &thread) const {
  if (thread.threadId.empty() || thread.activePlanId.empty()) {
    return std::nullopt;
  }

  try {
    firmius::core::ThreadManager tm(firmiusThreadsPath());
    return tm.getPlan(thread.threadId, thread.activePlanId);
  } catch (...) {
    return std::nullopt;
  }
}

ftxui::Component TuiState::root() {
  if (root_component_)
    return root_component_;

  auto title_bar = TitleBar(title_model_);
  auto status_bar = StatusBar(status_model_);
  auto agent_strip = AgentStrip(agent_strip_model_);
  auto plan_lane = PlanLane(plan_lane_model_);
  auto todo_lane = TodoLane(todo_lane_model_);

  auto input_bar = InputBar(
      input_model_,
      [this](const std::string &text,
             const std::vector<firmius::tui::PastedBlock> &images) {
        // Convert pasted image blocks to ImageContent
        std::vector<firmius::shared::ImageContent> image_contents;
        for (const auto &img : images) {
          if (img.type == "image") {
            firmius::shared::ImageContent content;
            content.url = "data:" + img.mime_type + ";base64," + img.content;
            content.mediaType = img.mime_type;
            content.detail = "auto";
            image_contents.push_back(content);
          }
        }

        if (!text.empty() && text[0] == '/') {
          CommandCtx ctx{this};
          auto &cmdManager = firmius::tui::CommandManager::instance();

          // Check if this is a workflow command before executing
          bool is_workflow_command = false;
          std::string content = text.substr(1);
          size_t space_pos = content.find(' ');
          std::string cmd_name = (space_pos == std::string::npos)
                                     ? content
                                     : content.substr(0, space_pos);

          auto it = cmdManager.getCommand(cmd_name);
          if (it && it->isWorkflow()) {
            is_workflow_command = true;
          }

          if (cmdManager.executeCommand(ctx, text)) {
            // Command handled successfully
            // If on welcome screen and this is a workflow command, create thread
            // and switch to chat mode
            if (view_mode_ == ViewMode::Welcome && harness_ &&
                is_workflow_command) {
              std::string cwd = std::filesystem::current_path().string();
              std::string newThreadId = harness_->newThread(
                  {}, cwd, resolveDefaultLeadPersona(harness_));

              // Sync UI with the newly created lead agent
              auto agents = harness_->listAgents(newThreadId);
              if (!agents.empty()) {
                focused_agent_id_ = agents.front();
                history_ = harness_->getAgentHistoryPtr(focused_agent_id_);
                if (chat_component_) {
                  chat_component_->OnEvent(
                      ftxui::Event::Special("ThreadChanged"));
                }
              }
              setViewMode(ViewMode::Chat);
            }
            return;
          }
        }

        if (view_mode_ == ViewMode::Welcome) {
          // If we are on the welcome screen, typing a message automatically
          // starts a thread
          if (harness_) {
            // Auto-create thread in current directory with default lead persona
            std::string cwd = std::filesystem::current_path().string();
            std::string newThreadId = harness_->newThread(
                {}, cwd, resolveDefaultLeadPersona(harness_));
            harness_->send(text, image_contents);

            // Immediately sync UI with the newly created lead agent
            auto agents = harness_->listAgents(newThreadId);
            if (!agents.empty()) {
              focused_agent_id_ = agents.front();
              history_ = harness_->getAgentHistoryPtr(focused_agent_id_);
              if (chat_component_) {
                chat_component_->OnEvent(
                    ftxui::Event::Special("ThreadChanged"));
              }
            }
          }
          setViewMode(ViewMode::Chat);
        } else {
          if (harness_) {
            harness_->send(text, image_contents);
          }
        }
        input_component_->TakeFocus();
      });
  auto focused_history_getter = [this]() -> const firmius::shared::AgentHistory * {
    if (focused_agent_id_.empty()) {
      return nullptr;
    }
    auto agent =
        firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
    if (!agent) {
      return history_.get();
    }
    return agent->getContext().history.get();
  };

  auto chat = ChatWindow(
      focused_history_getter,
      [this, focused_history_getter]() {
        std::vector<ftxui::Element> live_rows;
        const auto *s = stream_state_.getStream(focused_agent_id_);

        auto decorateMsg = [](const ftxui::Element &content) {
          return ftxui::hbox({ftxui::text("* ") | ftxui::bold |
                                  ftxui::color(ftxui::Color::Yellow),
                              content | ftxui::flex}) |
                 ftxui::flex;
        };

        auto full_width_separator = [](const std::string &label) {
          int width = ftxui::Terminal::Size().dimx;
          std::string label_text = " " + label + " ";
          if (width <= static_cast<int>(label_text.size())) {
            return ftxui::text(label_text) | ftxui::dim | ftxui::center |
                   ftxui::flex;
          }
          int left = (width - static_cast<int>(label_text.size())) / 2;
          int right = width - static_cast<int>(label_text.size()) - left;
          std::string line =
              std::string(left, '-') + label_text + std::string(right, '-');
          return ftxui::text(line) | ftxui::dim | ftxui::flex;
        };

        if (s) {

          auto compaction_separator = [&full_width_separator]() {
            return full_width_separator("Compaction");
          };
          bool has_compaction_output =
              s->compaction_active || !s->compaction_thinking.empty() ||
              !s->compaction_text.empty();
          if (has_compaction_output) {
            live_rows.push_back(compaction_separator());
            if (!s->compaction_thinking.empty()) {
              live_rows.push_back(decorateMsg(
                  firmius::tui::RenderMarkdown(s->compaction_thinking, true)));
            }
            if (!s->compaction_text.empty()) {
              live_rows.push_back(decorateMsg(
                  firmius::tui::RenderMarkdown(s->compaction_text)));
            }
          }

          if (s->provider_waiting) {
            // Eliminated diagnostic provider waiting text
          }
        }

        const auto &timeline = stream_state_.getTimeline();
        const auto &tool_calls = stream_state_.getToolCalls();
        const auto persisted_tool_call_ids =
            firmius::tui::CollectToolCallIdsFromHistory(
                focused_history_getter());

        // Find parent tool calls that spawned this focused subagent
        std::unordered_set<std::string> parent_tool_calls_for_subagent;
        for (const auto &[toolCallId, view] : tool_calls) {
          if (view && view->name == "summon_subagent" &&
              view->subagent_id == focused_agent_id_) {
            parent_tool_calls_for_subagent.insert(toolCallId);
          }
        }

        for (const auto &entry : timeline) {
          if (entry.kind == TimelineEntry::Kind::Thinking ||
              entry.kind == TimelineEntry::Kind::Text) {
            if (entry.agentId != focused_agent_id_) {
              continue;
            }
            live_rows.push_back(decorateMsg(firmius::tui::RenderMarkdown(
                entry.message, entry.kind == TimelineEntry::Kind::Thinking)));
            continue;
          }

          if (entry.kind != TimelineEntry::Kind::ToolCall) {
            continue;
          }

          // Show tool calls from focused agent OR tool calls that spawned this
          // subagent.
          bool is_focused_agent_tool = (entry.agentId == focused_agent_id_);
          bool is_parent_tool_for_subagent =
              (parent_tool_calls_for_subagent.count(entry.id) > 0);

          if (!is_focused_agent_tool && !is_parent_tool_for_subagent) {
            continue;
          }

          auto it_tool = tool_calls.find(entry.id);
          if (it_tool == tool_calls.end() || !it_tool->second) {
            continue;
          }

          if (persisted_tool_call_ids.count(entry.id) > 0) {
            continue;
          }

          if (!firmius::tui::ShouldRenderToolCallView(*it_tool->second)) {
            continue;
          }

          const auto descriptor =
              firmius::tui::DescribeQuickToolCall(*it_tool->second);
          if (firmius::tui::IsQuickToolCategory(descriptor.category)) {
            continue;
          }

          auto sub_history_getter = [this](const std::string &agentId)
              -> const firmius::shared::AgentHistory * {
            if (agentId.empty())
              return nullptr;
            return harness_->getAgentHistoryPtr(agentId).get();
          };
          auto sub_stream_getter = [this](const std::string &agentId)
              -> const firmius::tui::StreamState * {
            if (agentId.empty())
              return nullptr;
            return stream_state_.getStream(agentId);
          };

          live_rows.push_back(
              decorateMsg(ToolBlock(it_tool->second, sub_history_getter,
                                    sub_stream_getter)
                              ->Render()));
        }

        const auto &queued = stream_state_.getQueuedMessages();
        if (!queued.empty()) {
          auto queued_separator = [&full_width_separator]() {
            return full_width_separator("Queued Messages");
          };
          live_rows.push_back(queued_separator());
          for (const auto &[id, text] : queued) {
            live_rows.push_back(
                ftxui::hbox({ftxui::text("> ") | ftxui::bold |
                                 ftxui::color(ftxui::Color::Cyan),
                             firmius::tui::RenderMarkdown(text) |
                                 ftxui::dim | ftxui::flex}) |
                ftxui::flex);
          }
        }

        return live_rows;
      },
      [this](const std::string &toolCallId) {
        return stream_state_.getToolView(toolCallId);
      },
      [this](
          const std::string &agentId) -> const firmius::shared::AgentHistory * {
        if (agentId.empty())
          return nullptr;
        return harness_->getAgentHistoryPtr(agentId).get();
      },
      [this](const std::string &agentId) -> const firmius::tui::StreamState * {
        if (agentId.empty())
          return nullptr;
        return stream_state_.getStream(agentId);
      },
      [this, focused_history_getter]() {
        std::unordered_map<int, firmius::tui::LiveQuickSummaryCluster> clusters;
        std::vector<int> cluster_order;
        const auto &timeline = stream_state_.getTimeline();
        const auto &tool_calls = stream_state_.getToolCalls();
        const auto persisted_tool_call_ids =
            firmius::tui::CollectToolCallIdsFromHistory(
                focused_history_getter());

        std::unordered_set<std::string> parent_tool_calls_for_subagent;
        for (const auto &[toolCallId, view] : tool_calls) {
          if (view && view->name == "summon_subagent" &&
              view->subagent_id == focused_agent_id_) {
            parent_tool_calls_for_subagent.insert(toolCallId);
          }
        }

        for (const auto &entry : timeline) {
          const bool is_focused_agent_tool = (entry.agentId == focused_agent_id_);
          const bool is_parent_tool_for_subagent =
              (parent_tool_calls_for_subagent.count(entry.id) > 0);
          if (!is_focused_agent_tool && !is_parent_tool_for_subagent) {
            continue;
          }
          if (entry.kind != TimelineEntry::Kind::ToolCall) {
            continue;
          }

          auto it_tool = tool_calls.find(entry.id);
          if (it_tool == tool_calls.end() || !it_tool->second) {
            continue;
          }

          const auto &view = it_tool->second;
          if (persisted_tool_call_ids.count(entry.id) > 0) {
            continue;
          }

          if (!firmius::tui::ShouldRenderToolCallView(*view)) {
            continue;
          }

          const auto descriptor = firmius::tui::DescribeQuickToolCall(*view);
          if (!firmius::tui::IsQuickToolCategory(descriptor.category)) {
            continue;
          }

          int cluster_id = stream_state_.getToolCallClusterId(entry.id);
          if (cluster_id < 0) {
            cluster_id = 0;
          }
          if (!clusters.count(cluster_id)) {
            cluster_order.push_back(cluster_id);
          }
          auto &cluster = clusters[cluster_id];
          cluster.merge_with_history = (cluster_id == 0);

          auto key = static_cast<int>(descriptor.category);
          auto &summary = cluster.summaries[key];
          if (summary.category == firmius::tui::QuickToolCategory::None) {
            summary.category = descriptor.category;
            cluster.category_order.push_back(descriptor.category);
          }
          if (!descriptor.target.empty()) {
            summary.targets.push_back(descriptor.target);
          }
          if (view->phase == ToolPhase::Preparing) {
            summary.has_preparing = true;
            summary.preparing_count++;
          } else if (view->phase == ToolPhase::Called) {
            summary.has_live = true;
            summary.live_count++;
          } else if (view->phase == ToolPhase::Error) {
            summary.has_error = true;
          }
        }

        std::vector<firmius::tui::LiveQuickSummaryCluster> result;
        result.reserve(cluster_order.size());
        for (int cluster_id : cluster_order) {
          result.push_back(std::move(clusters[cluster_id]));
        }
        return result;
      });
  chat_component_ = chat;
  input_component_ = input_bar;

  auto container = ftxui::Container::Vertical({
      input_bar,
      chat,
      todo_lane,
      plan_lane,
      agent_strip,
  });

  auto welcome_screen = ftxui::Renderer([] {
    return ftxui::vbox({
               ftxui::text("Welcome to Firmius") | ftxui::bold | ftxui::center,
               ftxui::text("Type a message to start") | ftxui::dim |
                   ftxui::center,
           }) |
           ftxui::flex | ftxui::center;
  });

  auto base_view =
      ftxui::Renderer(container, [this, title_bar, status_bar, plan_lane,
                                  todo_lane, agent_strip, input_bar, chat,
                                  welcome_screen] {
        // Deferred modal clearing: drain here where it's safe
        if (pending_modal_clear_) {
          modals_.clear();
          pending_modal_clear_ = false;
        }

        // AGGRESSIVELY take focus if no modals are open
        if (modals_.empty() && input_component_) {
          input_component_->TakeFocus();
        }

        ftxui::Element chat_area;
        if (view_mode_ == ViewMode::Chat) {
          chat_area = chat->Render() | ftxui::flex;
        } else if (view_mode_ == ViewMode::ProcessFocus) {
          // Process focus mode - show process output prominently
          chat_area = chat->Render() | ftxui::flex;
        } else {
          chat_area = welcome_screen->Render();
        }

        const auto &theme = ThemeManager::instance().getCurrentTheme();
        const auto terminal = ftxui::Terminal::Size();
        bool isLead = false;
        bool isExecutor = false;
        if (!focused_agent_id_.empty()) {
          auto focusedAgent =
              firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
          if (focusedAgent) {
            const auto &persona = focusedAgent->getContext().config.personaName;
            isLead = (persona == "lead");
            isExecutor = (persona == "executor");
          }
        }
        const bool hasPlan = plan_lane_model_ && plan_lane_model_->visible;
        const bool hasTodo = todo_lane_model_ && todo_lane_model_->visible;
        const bool hasExecutorChunk =
            hasTodo && todo_lane_model_->show_chunk_header;
        const auto panelDecision = determineWorkPanelDecision(
            isLead, isExecutor, hasPlan, hasTodo, hasExecutorChunk,
            terminal.dimx, terminal.dimy, prefer_todo_panel_on_narrow_);

        ftxui::Element work_panel = ftxui::text("");
        bool show_work_panel = false;
        if (panelDecision.kind == WorkPanelKind::SplitPlanTodo) {
          work_panel = ftxui::hbox({
                           plan_lane->Render() | ftxui::flex,
                           ftxui::separator() | ftxui::color(theme.base.border),
                           todo_lane->Render() | ftxui::flex,
                       }) |
                       ftxui::xflex;
          show_work_panel = true;
        } else if (panelDecision.kind == WorkPanelKind::SingleToggle) {
          auto activeLabel = ftxui::text(" PANEL: " + panelDecision.activePanelLabel +
                                         " (Ctrl+O to toggle) ") |
                             ftxui::bold | ftxui::color(theme.base.bg) |
                             ftxui::bgcolor(theme.base.highlight);
          work_panel =
              ftxui::vbox({activeLabel | ftxui::xflex,
                           panelDecision.showPlan ? plan_lane->Render()
                                                  : todo_lane->Render()}) |
              ftxui::xflex;
          show_work_panel = true;
        } else if (panelDecision.kind == WorkPanelKind::ExecutorChunkTodo ||
                   panelDecision.kind == WorkPanelKind::TodoOnly) {
          work_panel = todo_lane->Render();
          show_work_panel = true;
        } else if (panelDecision.kind == WorkPanelKind::PlanOnly) {
          work_panel = plan_lane->Render();
          show_work_panel = true;
        }

        // Ultra-compact bottom bar layout
        ftxui::Elements bottom_bar_children;
        bottom_bar_children.push_back(agent_strip->Render());
        bottom_bar_children.push_back(ftxui::separator() |
                                      ftxui::color(theme.base.border));
        bottom_bar_children.push_back(input_bar->Render());
        bottom_bar_children.push_back(ftxui::separator() |
                                      ftxui::color(theme.base.border));
        bottom_bar_children.push_back(status_bar->Render());

        auto bottom_bar = ftxui::vbox(std::move(bottom_bar_children));

        ftxui::Element main_view;
        if (view_mode_ == ViewMode::Welcome) {
          main_view = ftxui::vbox({chat_area, bottom_bar}) | ftxui::flex;
        } else {
          if (show_work_panel) {
            main_view = ftxui::vbox({
                            title_bar->Render(),
                            chat_area | ftxui::flex,
                            ftxui::separator() | ftxui::color(theme.base.border),
                            work_panel,
                            bottom_bar,
                        }) |
                        ftxui::flex;
          } else {
            main_view = ftxui::vbox({
                            title_bar->Render(),
                            chat_area | ftxui::flex,
                            bottom_bar,
                        }) |
                        ftxui::flex;
          }
        }

        // Layer notifications
        auto notifications = NotificationManager::instance().render();
        main_view = ftxui::dbox({main_view, notifications});

        return main_view | ftxui::bgcolor(theme.base.bg);
      });

  // Layer modals using dbox
  auto modal_renderer = ftxui::Renderer(base_view, [this, base_view]() {
    ftxui::Element current = base_view->Render();
    for (const auto &modal : modals_) {
      current = ftxui::dbox(
          {DarkenElement(current),
           modal->Render() | ftxui::clear_under | ftxui::center});
    }
    return current;
  });

  root_component_ = ftxui::CatchEvent(modal_renderer, [this, chat](
                                                          ftxui::Event event) {
    if (event == ftxui::Event::Custom) {
      if (!deferred_ui_mutations_.empty()) {
        auto deferred = std::move(deferred_ui_mutations_);
        deferred_ui_mutations_.clear();
        for (auto &mutation : deferred) {
          mutation();
        }
      }
      drainEvents();
      return true;
    }

    if (event == ftxui::Event::Special("PopModal")) {
      if (!modals_.empty()) {
        modals_.pop_back();
      }
      if (modals_.empty()) {
        if (input_component_) {
          input_component_->TakeFocus();
        }
        // Force re-render to ensure focus state is propagated
        postEvent(ftxui::Event::Custom);
      } else {
        modals_.back()->TakeFocus();
      }
      return true;
    }

    // Handle terminal focus gained event (escape sequence \x1b[I)
    if (event.input() == "\x1b[I") {
      if (input_model_)
        input_model_->is_focused = true;
      // Terminal regained focus - restore focus to input
      if (input_component_) {
        input_component_->TakeFocus();
      }
      return true;
    }

    // Handle terminal focus lost event (escape sequence \x1b[O)
    if (event.input() == "\x1b[O") {
      if (input_model_)
        input_model_->is_focused = false;
      return true;
    }

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
    if (IsPermissionCycleEvent(event)) { // Ctrl+Y (Permission Mode)
      cycleThreadPermissionMode();
      return true;
    }
    if (IsRetryLastRequestEvent(event)) {
      if (!harness_) {
        return true;
      }
      std::string statusMessage;
      if (harness_->retryLastRequest(statusMessage)) {
        NotificationManager::instance().notifyInfo(
            "Retry Request", statusMessage, std::chrono::milliseconds(1800));
      } else {
        NotificationManager::instance().notifyWarning(
            "Retry Request", statusMessage, std::chrono::milliseconds(1800));
      }
      return true;
    }
    if (event.input() == "\x1b[Z") { // Shift+Tab (cycle lead mode)
      if (!harness_)
        return true;

      if (!focused_agent_id_.empty()) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (agent) {
          const auto &ctx = agent->getContext();
          if (!ctx.identity.parentId.empty()) {
            NotificationManager::instance().notifyWarning(
                "Lead Mode",
                "Cannot switch mode while focused on a subagent.",
                std::chrono::milliseconds(1500));
            return true;
          }
          if (agent->isRunning() || agent->isBooting()) {
            NotificationManager::instance().notifyWarning(
                "Lead Mode",
                "Lead agent is busy. Cancel or wait before switching.",
                std::chrono::milliseconds(1500));
            return true;
          }
        }
      }

      auto modes = getSwitchableLeadPersonas();
      std::string current;
      if (!focused_agent_id_.empty()) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (agent) {
          current = agent->getContext().identity.name;
        }
      }
      if (current.empty() && !thread_.leadPersona.empty()) {
        current = thread_.leadPersona;
      }
      if (current.empty()) {
        current = resolveDefaultLeadPersona(harness_);
      }

      size_t next_index = 0;
      auto it = std::find(modes.begin(), modes.end(), current);
      if (it != modes.end()) {
        next_index =
            (static_cast<size_t>(it - modes.begin()) + 1) % modes.size();
      }
      std::string next = modes[next_index];

      auto cfg = harness_->getConfig();
      cfg.defaultLeadPersona = next;
      harness_->updateConfig(cfg);
      harness_->saveConfig();

      bool switched = false;
      if (!thread_.threadId.empty() &&
          harness_->currentThreadId() == thread_.threadId) {
        switched = harness_->switchLeadPersona(next);
        if (switched) {
          focused_agent_id_ = harness_->focusedAgentId();
          history_ = focused_agent_id_.empty()
                         ? nullptr
                         : harness_->getAgentHistoryPtr(focused_agent_id_);
        }
      }
      thread_.leadPersona = next;

      updateStatusModel();
      updateAgentStripModel();
      updateTodoLaneModel();
      if (chat_component_) {
        chat_component_->OnEvent(ftxui::Event::Special("ThreadChanged"));
      }
      if (screen_) {
        screen_->PostEvent(ftxui::Event::Custom);
      }

      NotificationManager::instance().notifyInfo(
          "Lead Mode", "Lead persona: " + resolvePersonaTitle(next),
          std::chrono::milliseconds(1500));
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
            updateStatusModel();
            updateAgentStripModel();
            updateTodoLaneModel();
            if (screen_)
              screen_->PostEvent(ftxui::Event::Custom);
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
              updateStatusModel();
              updateAgentStripModel();
              updateTodoLaneModel();
              if (screen_)
                screen_->PostEvent(ftxui::Event::Custom);
            }
          }
        }
      }
      return true;
    }

    if (event == ftxui::Event::Special("\x14")) { // Ctrl+T (Cycle Theme)
      ThemeManager::instance().cycleTheme();
      if (chat_component_) {
        chat_component_->OnEvent(ftxui::Event::Special("ThemeChanged"));
      }
      NotificationManager::instance().notifyInfo(
          "Theme Changed", ThemeManager::instance().getCurrentTheme().name,
          std::chrono::milliseconds(1500));
      return true;
    }

    if (event ==
        ftxui::Event::Special("\x19")) { // Ctrl+Y (Cycle model variant)
      if (harness_ && !focused_agent_id_.empty()) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (agent) {
          auto &ctx =
              const_cast<firmius::shared::AgentContext &>(agent->getContext());
          auto provider =
              firmius::provider::ProviderRegistry::instance().getProvider(
                  ctx.config.providerId);
          if (provider) {
            auto info = provider->getModelInfo(ctx.config.modelId);
            if (!info.variants.empty()) {
              std::string current = ctx.config.modelVariant;
              std::string next = "";
              if (current.empty()) {
                next = info.variants.front().variantName;
              } else {
                auto it = std::find_if(
                    info.variants.begin(), info.variants.end(),
                    [&](const auto &v) { return v.variantName == current; });
                if (it != info.variants.end() &&
                    std::next(it) != info.variants.end()) {
                  next = std::next(it)->variantName;
                }
              }
              ctx.config.modelVariant = next;
              updateStatusModel();
              if (chat_component_) {
                chat_component_->OnEvent(
                    ftxui::Event::Special("ThreadChanged"));
              }
            }
          }
        }
      }
      return true;
    }

    // Ctrl+H - Toggle notifications
    if (event == ftxui::Event::Special("\x08")) {
      NotificationManager::instance().toggleVisibility();
      return true;
    }

    // Ctrl+O - Toggle plan lane expansion
    if (event == ftxui::Event::Special("\x0F")) {
      const auto terminal = ftxui::Terminal::Size();
      bool isLead = false;
      bool isExecutor = false;
      if (!focused_agent_id_.empty()) {
        auto focusedAgent =
            firmius::core::AgentRegistry::instance().getAgent(focused_agent_id_);
        if (focusedAgent) {
          const auto &persona = focusedAgent->getContext().config.personaName;
          isLead = (persona == "lead");
          isExecutor = (persona == "executor");
        }
      }
      const bool hasPlan = plan_lane_model_ && plan_lane_model_->visible;
      const bool hasTodo = todo_lane_model_ && todo_lane_model_->visible;
      const bool hasExecutorChunk =
          hasTodo && todo_lane_model_->show_chunk_header;
      const auto panelDecision = determineWorkPanelDecision(
          isLead, isExecutor, hasPlan, hasTodo, hasExecutorChunk,
          terminal.dimx, terminal.dimy, prefer_todo_panel_on_narrow_);
      if (panelDecision.kind == WorkPanelKind::SingleToggle) {
        prefer_todo_panel_on_narrow_ = !prefer_todo_panel_on_narrow_;
      } else {
        plan_lane_expanded_ = !plan_lane_expanded_;
        active_plan_state_.setExpanded(plan_lane_expanded_);
        updatePlanLaneModel();
      }
      if (screen_) {
        screen_->PostEvent(ftxui::Event::Custom);
      }
      return true;
    }

    // Ctrl+G - Toggle diff expansion
    if (event == ftxui::Event::Special("\x07")) {
      diffs_expanded_ = !diffs_expanded_;
      UIState::instance().diffsExpanded = diffs_expanded_;

      if (diffs_expanded_) {
        NotificationManager::instance().notifyInfo(
            "Diffs Expanded", "Showing full diff content",
            std::chrono::milliseconds(2000));
      } else {
        NotificationManager::instance().notifyInfo(
            "Diffs Collapsed", "Showing top 3 relevant hunks (10 lines max)",
            std::chrono::milliseconds(2000));
      }
      return true;
    }

    // ? / F1 - Open help overlay
    if (event == ftxui::Event::Character("?")) {
      if (input_.empty() && cursor_ == 0) {
        openModalDirect(HelpOverlay(*this));
        return true;
      }
    }
    if (event == ftxui::Event::F1) {
      if (modals_.empty()) {
        openModalDirect(HelpOverlay(*this));
        return true;
      }
    }

    // Ctrl+F - Focus on process (interactive mode)
    if (event == ftxui::Event::Special("\x06")) {
      if (!focused_agent_id_.empty()) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(
            focused_agent_id_);
        if (agent) {
          const auto &ctx = agent->getContext();
          if (!ctx.state.ownedProcesses.empty()) {
            focused_process_id_ = ctx.state.ownedProcesses.front();
            view_mode_ = ViewMode::ProcessFocus;
            NotificationManager::instance().notifyInfo(
                "Process Focus",
                "Interactive mode enabled. Type to send input to process.",
                std::chrono::milliseconds(3000));
            return true;
          }
        }
      }
      NotificationManager::instance().notifyWarning(
          "No Process", "No active background process to focus on.",
          std::chrono::milliseconds(2000));
      return true;
    }

    // Exit process focus mode with Escape
    if (event == ftxui::Event::Escape && view_mode_ == ViewMode::ProcessFocus) {
      view_mode_ = ViewMode::Chat;
      focused_process_id_.clear();
      NotificationManager::instance().notifyInfo(
          "Process Focus", "Exited process focus mode.",
          std::chrono::milliseconds(2000));
      return true;
    }

    return false;
  });

  return root_component_;
}

} // namespace firmius::tui
