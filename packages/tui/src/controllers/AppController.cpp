#include "controllers/AppController.hpp"
#include "Engine.hpp"
#include "Events.hpp"
#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "controllers/PermissionController.hpp"
#include "controllers/TranscriptController.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include <type_traits>
#include <variant>

namespace firmius::tui {

AppController &AppController::instance() {
  static AppController inst;
  return inst;
}

void AppController::dispatch(const firmius::shared::AppEvent &event,
                             firmius::core::Harness *harness) {
  auto &store = TUIStore::instance();
  auto &state = TuiState::instance();
  const auto current_thread_id = [&]() -> const std::string & {
    return state.thread_.threadId.empty() ? store.thread_id
                                          : state.thread_.threadId;
  };
  const auto current_focused_agent_id = [&]() -> const std::string & {
    return state.focused_agent_id_.empty() ? store.focused_agent_id
                                           : state.focused_agent_id_;
  };

  std::visit(
      [&](auto &&e) {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, shared::AgentThinking>) {
          state.stream_state_.handleAgentThinking(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentProviderWaiting>) {
          state.stream_state_.handleAgentProviderWaiting(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentText>) {
          state.stream_state_.handleAgentText(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::ConfigUpdated>) {
          state.requestRefresh(RefreshFlags::Status);
        } else if constexpr (std::is_same_v<T, shared::AgentTurnCompleted>) {
          state.stream_state_.handleAgentTurnCompleted(e);
          if (e.agentId == current_focused_agent_id()) {
            session_metrics_ += e.turn.metrics;
          }
          state.refreshFocusedHistory();
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.requestRefresh(RefreshFlags::TodoLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentToolCallChunk>) {
          state.stream_state_.handleAgentToolCallChunk(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentToolCall>) {
          state.stream_state_.handleAgentToolCall(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentFileEdited>) {
          state.stream_state_.handleAgentFileEdited(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::ThreadChanged>) {
          auto threads = harness->listThreads();

          auto thread = std::find_if(threads.begin(), threads.end(),
                                     [&](const shared::ThreadMetadata &t) {
                                       return t.threadId == e.threadId;
                                     });

          auto all_agents = harness->listAgents(e.threadId);
          auto if_lead_welcome = all_agents.size() == 1 ? true : false;

          if (if_lead_welcome) {
            state.thread_ = *thread;
            state.applyThreadOpened(*thread, all_agents[0], false);
            state.focusAgent(all_agents[0]);
          }

          if (e.threadId == current_thread_id()) {
            state.stream_state_.handleThreadChanged();
            state.requestRefresh(RefreshFlags::Status);
            state.requestRefresh(RefreshFlags::AgentStrip);
            state.requestRefresh(RefreshFlags::PlanLane);
            state.requestRefresh(RefreshFlags::TodoLane);
            state.requestRefresh(RefreshFlags::ContextLane);
            state.requestRefresh(RefreshFlags::ChatTranscript);

            state.refreshFocusedHistory();
            state.notifyChatTranscriptChanged();
            state.applyPendingRefreshes();
          }

        } else if constexpr (std::is_same_v<T, shared::ThreadMetadataUpdated>) {
          if (e.threadId == current_thread_id()) {
            store.thread_metadata = e.metadata;
            state.thread_ = e.metadata;
            state.requestRefresh(RefreshFlags::Status);
          }
        } else if constexpr (std::is_same_v<
                                 T, shared::PermissionEscalationRequest>) {
          PermissionController::instance().activateRequest(e);
        } else if constexpr (std::is_same_v<
                                 T, shared::PermissionEscalationResolved>) {
          PermissionController::instance().promoteNextRequest();
        } else if constexpr (std::is_same_v<T, shared::AgentSpawned>) {
          if (e.parentId.empty()) {
            // state.focusAgent(e.agentId);
          }
          state.stream_state_.handleAgentSpawned(e, current_focused_agent_id());
          state.requestRefresh(RefreshFlags::AgentStrip);
          // Reset live-row phrase so the new agent gets a fresh phrase
          // appropriate to its current activity.
          if (e.agentId == current_focused_agent_id() ||
              current_focused_agent_id().empty()) {
            state.live_row_current_phrase_.clear();
          }
        } else if constexpr (std::is_same_v<T, shared::HistoryUndone>) {
          if (e.threadId == current_thread_id() &&
              (e.agentId.empty() || e.agentId == current_focused_agent_id())) {
            state.refreshFocusedHistory();
            state.requestRefresh(RefreshFlags::Status);
            TranscriptController::instance()
                .expandHistoryForTranscriptIfNeeded();
            state.requestRefresh(RefreshFlags::AgentStrip);
          }
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentError>) {
          state.stream_state_.handleAgentError(e);

          if (!e.message.empty()) {
            NotificationManager::instance().notifyError("Agent Error",
                                                        e.message, false);
          }
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentFinished>) {
          state.stream_state_.handleAgentFinished(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentInterrupted>) {
          state.stream_state_.handleAgentInterrupted(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::ThreadTitleUpdated>) {
          store.thread_metadata.title = e.title;
          state.thread_.title = e.title;
        } else if constexpr (std::is_same_v<T, shared::MessageQueued>) {
          state.stream_state_.handleMessageQueued(e);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::MessageDequeued>) {
          state.stream_state_.handleMessageDequeued(e);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::InternalMessageQueued>) {
          state.stream_state_.handleInternalMessageQueued(e);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T,
                                            shared::InternalMessageDequeued>) {
          state.stream_state_.handleInternalMessageDequeued(e);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentProcessSpawned>) {
          state.stream_state_.handleAgentProcessSpawned(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentProcessOutput>) {
          state.stream_state_.handleAgentProcessOutput(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::ModelSwitched>) {
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentAccountSwitched>) {
          state.stream_state_.handleAgentAccountSwitched(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.requestRefresh(RefreshFlags::ContextLane);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentRetrying>) {
          state.stream_state_.handleAgentRetrying(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentRetryFailed>) {
          state.stream_state_.handleAgentRetryFailed(e);
          state.requestRefresh(RefreshFlags::AgentStrip);
          state.requestRefresh(RefreshFlags::Status);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::ThreadLocked>) {
          state.openModal("ThreadLocked");
        } else if constexpr (std::is_same_v<T, shared::AgentCompacting>) {
          state.stream_state_.handleAgentCompacting(e);
          state.requestRefresh(RefreshFlags::Status);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T,
                                            shared::AgentCompactionThinking>) {
          state.stream_state_.handleAgentCompactionThinking(e);
          state.requestRefresh(RefreshFlags::Status);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::AgentCompactionText>) {
          state.stream_state_.handleAgentCompactionText(e);
          state.requestRefresh(RefreshFlags::Status);
          state.notifyChatTranscriptChanged();
        } else if constexpr (std::is_same_v<T, shared::ContextCompacted>) {
          if (e.agentId == current_focused_agent_id()) {
            // Reset phrase state on compaction to transition to "compacted"
            // vibe
            state.live_row_current_phrase_.clear();
          }
          state.stream_state_.handleContextCompacted(e);
          if (e.agentId.empty() || e.agentId == current_focused_agent_id()) {
            state.refreshFocusedHistory();
            TranscriptController::instance()
                .expandHistoryForTranscriptIfNeeded();
            state.requestRefresh(RefreshFlags::ContextLane);
            state.requestRefresh(RefreshFlags::Status);
            state.notifyChatTranscriptChanged();
          }
        } else if constexpr (std::is_same_v<T, shared::UserMessageSent>) {
          const bool thread_matches =
              e.threadId == current_thread_id() ||
              (harness && !e.threadId.empty() &&
               harness->currentThreadId() == e.threadId);
          if (thread_matches) {
            if (current_thread_id().empty()) {
              store.thread_id = e.threadId;
              state.thread_.threadId = e.threadId;
            }
            shared::AgentTurn turn;
            if (!state.history_) {
              state.history_ = std::make_shared<shared::AgentHistory>();
              state.history_->threadId = state.thread_.threadId.empty()
                                             ? store.thread_id
                                             : state.thread_.threadId;
            }
            shared::Message msg;
            msg.id = e.messageId;
            msg.role = shared::Role::User;
            msg.content = {shared::TextContent{e.text}};
            for (const auto &image : e.images) {
              msg.content.push_back(image);
            }
            turn.messages.push_back(std::move(msg));
            state.history_->turns.push_back(std::move(turn));
            TranscriptModel::instance().active_history = state.history_;
            state.agent_history_cache_[current_focused_agent_id()] =
                state.history_;
            state.notifyChatTranscriptChanged();
          }
        }
      },
      event);
}

} // namespace firmius::tui
