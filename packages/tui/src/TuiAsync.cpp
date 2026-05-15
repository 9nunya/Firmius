#include "TUIState.hpp"

#include "NotificationManager.hpp"
#include "harness/Harness.hpp"

#include <chrono>
#include <filesystem>
#include <utility>

namespace firmius::tui {

namespace {

BackgroundTaskPool &ensureBackgroundTaskPool(TuiState &state) {
  if (!state.background_task_pool_) {
    state.background_task_pool_ = std::make_unique<BackgroundTaskPool>(4);
  }
  return *state.background_task_pool_;
}

UiTaskScheduler &ensureUiTaskScheduler(TuiState &state) {
  if (!state.ui_task_scheduler_) {
    state.ui_task_scheduler_ = std::make_unique<UiTaskScheduler>(
        ensureBackgroundTaskPool(state),
        [&state](std::function<void()> action) {
          state.deferUiMutation(std::move(action));
        });
  }
  return *state.ui_task_scheduler_;
}

std::string uniqueTaskKey(std::string prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return prefix + ":" +
         std::to_string(
             std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::shared_ptr<firmius::shared::AgentHistory>
ensureTranscriptHistory(std::shared_ptr<firmius::shared::AgentHistory> &history,
                        const std::string &thread_id) {
  if (!history) {
    history = std::make_shared<firmius::shared::AgentHistory>();
  }
  if (history->threadId.empty()) {
    history->threadId = thread_id;
  }
  return history;
}

} // namespace

void TuiState::runBackgroundTask(std::function<void()> action) {
  if (!action) {
    return;
  }
  ensureBackgroundTaskPool(*this).post(std::move(action));
}

void TuiState::appendOptimisticUserTurn(
    const std::string &text,
    const std::vector<firmius::shared::ImageContent> &images) {
  auto transcript_history = ensureTranscriptHistory(history_, thread_.threadId);
  shared::AgentTurn turn;
  turn.turnId =
      "user-live-ui-" +
      std::to_string(static_cast<long long>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count()));

  shared::Message message;
  message.id = turn.turnId;
  message.role = shared::Role::User;
  message.timestamp = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  message.content = {shared::TextContent{text}};
  for (const auto &image : images) {
    message.content.push_back(image);
  }
  turn.messages.push_back(std::move(message));
  transcript_history->turns.push_back(std::move(turn));
  if (!focused_agent_id_.empty()) {
    agent_history_cache_[focused_agent_id_] = transcript_history;
  }
  notifyChatTranscriptChanged();
  applyPendingRefreshes();
}

void TuiState::queuePromptSend(
    std::string text, std::vector<firmius::shared::ImageContent> images) {
  if (!harness_) {
    return;
  }

  setLoadingMessage("Sending message...");
  setLoadingProgress(0.15f);
  ensureUiTaskScheduler(*this).schedule(
      uniqueTaskKey("chat-send"), UiTaskScheduler::Priority::Interactive,
      [this, text = std::move(text), images = std::move(images)](
          const std::shared_ptr<std::atomic<bool>> &cancelled,
          std::uint64_t /*generation*/) mutable -> UiTaskScheduler::ApplyCallback {
        if (cancelled->load(std::memory_order_relaxed) || !harness_) {
          return {};
        }
        try {
          harness_->send(text, images);
          return [this]() { clearLoadingState(); };
        } catch (const std::exception &ex) {
          const std::string what = ex.what();
          return [this, what]() {
            clearLoadingState();
            NotificationManager::instance().notifyError("Send Failed", what,
                                                        false);
          };
        } catch (...) {
          return [this]() {
            clearLoadingState();
            NotificationManager::instance().notifyError(
                "Send Failed", "Unknown error.", false);
          };
        }
      });
}

void TuiState::submitPrompt(
    std::string text, std::vector<firmius::shared::ImageContent> images) {
  if (!harness_) {
    return;
  }

  const bool was_welcome = (view_mode_ == ViewMode::Welcome);
  appendOptimisticUserTurn(text, images);
  setViewMode(ViewMode::Chat);

  if (!was_welcome) {
    queuePromptSend(std::move(text), std::move(images));
    return;
  }

  setLoadingMessage("Starting thread...");
  setLoadingDetail("Bootstrapping lead agent and first turn.");
  setLoadingProgress(0.05f);

  ensureUiTaskScheduler(*this).schedule(
      "transition:welcome-send", UiTaskScheduler::Priority::Interactive,
      [this, text = std::move(text), images = std::move(images)](
          const std::shared_ptr<std::atomic<bool>> &cancelled,
          std::uint64_t /*generation*/) mutable -> UiTaskScheduler::ApplyCallback {
        if (cancelled->load(std::memory_order_relaxed) || !harness_) {
          return {};
        }
        try {
          const std::string cwd = std::filesystem::current_path().string();
          std::string lead = thread_.leadPersona;
          if (lead.empty()) {
            const auto &cfg = harness_->getConfig();
            lead = cfg.defaultLeadPersona.empty() ? std::string("lead")
                                                  : cfg.defaultLeadPersona;
          }
          harness_->newThread({}, cwd, lead);
          harness_->setCurrentThreadPermissionMode(thread_.permissionMode);
          harness_->send(text, images);

          const std::string opened_id = harness_->currentThreadId();
          const std::string focused = harness_->focusedAgentId();
          shared::ThreadMetadata metadata;
          bool found = false;
          if (!opened_id.empty()) {
            metadata = harness_->getThreadMetadata(opened_id);
            found = metadata.threadId == opened_id;
          }
          return [this, metadata, focused, found]() {
            if (found) {
              applyThreadOpened(metadata, focused, true);
            } else {
              clearLoadingState();
            }
          };
        } catch (const std::exception &ex) {
          const std::string what = ex.what();
          return [this, what]() {
            clearLoadingState();
            NotificationManager::instance().notifyError("Welcome Send Failed",
                                                        what, false);
          };
        } catch (...) {
          return [this]() {
            clearLoadingState();
            NotificationManager::instance().notifyError(
                "Welcome Send Failed", "Unknown error.", false);
          };
        }
      });
}

void TuiState::requestThreadOpen(std::optional<std::string> thread_id,
                                 bool resume_last,
                                 std::string loading_message,
                                 std::string loading_detail) {
  if (!loading_message.empty()) {
    setLoadingMessage(std::move(loading_message));
  }
  if (!loading_detail.empty()) {
    setLoadingDetail(std::move(loading_detail));
  }
  setLoadingProgress(0.1f);

  ensureUiTaskScheduler(*this).schedule(
      "transition:thread-open", UiTaskScheduler::Priority::Interactive,
      [this, thread_id = std::move(thread_id), resume_last](
          const std::shared_ptr<std::atomic<bool>> &cancelled,
          std::uint64_t /*generation*/) mutable -> UiTaskScheduler::ApplyCallback {
        if (cancelled->load(std::memory_order_relaxed) || !harness_) {
          return [this]() { clearLoadingState(); };
        }
        try {
          std::string opened_id;
          if (resume_last) {
            if (harness_->resumeLast()) {
              opened_id = harness_->currentThreadId();
            }
          } else if (thread_id.has_value() && !thread_id->empty()) {
            if (harness_->switchThread(*thread_id)) {
              opened_id = *thread_id;
            }
          }

          shared::ThreadMetadata metadata;
          bool found = false;
          if (!opened_id.empty()) {
            metadata = harness_->getThreadMetadata(opened_id);
            found = metadata.threadId == opened_id;
          }
          const std::string focused = harness_->focusedAgentId();
          return [this, metadata, focused, found]() {
            if (found) {
              applyThreadOpened(metadata, focused, true);
            } else {
              clearLoadingState();
            }
          };
        } catch (const std::exception &ex) {
          const std::string what = ex.what();
          return [this, what]() {
            clearLoadingState();
            NotificationManager::instance().notifyError(
                "Thread Open Failed", what, false);
          };
        } catch (...) {
          return [this]() {
            clearLoadingState();
            NotificationManager::instance().notifyError(
                "Thread Open Failed", "Unknown error.", false);
          };
        }
      });
}

std::unordered_map<std::string, UiTaskScheduler::Telemetry>
TuiState::uiTaskTelemetrySnapshot() const {
  if (!ui_task_scheduler_) {
    return {};
  }
  return ui_task_scheduler_->telemetrySnapshot();
}

} // namespace firmius::tui
