#include "ActionDispatcher.hpp"
#include "TranscriptRenderer.hpp"

#include <filesystem>

namespace firmius::tui2 {

ActionDispatcher::ActionDispatcher(DaemonSession &session, AppState &state)
    : session_(session), state_(state) {}

bool ActionDispatcher::sendMessage(const std::string &text) {
  if (text.empty()) return false;

  auto threadId = state_.threadId();
  auto agentId = state_.agentId();

  if (threadId.empty()) {
    // Auto-create a thread if none exists.
    if (!createThread()) return false;
    threadId = state_.threadId();
    agentId = state_.agentId();
  }



  auto response = session_.send(threadId, agentId, text);
  return response.accepted;
}

bool ActionDispatcher::createThread(const std::string &persona,
                                     const std::string &mode) {
  auto cwd = std::filesystem::current_path().string();
  auto response = session_.createThread(cwd, persona, mode);
  if (response.thread.threadId.empty()) return false;

  state_.setThreadId(response.thread.threadId);
  state_.setAgentId(response.focusedAgentId);
  state_.setThreadTitle(response.thread.title);
  state_.setTranscriptLines({});

  // Open the thread to establish focus.
  session_.openThread(response.thread.threadId);
  return true;
}

bool ActionDispatcher::openThread(const std::string &threadId) {
  auto response = session_.openThread(threadId);
  if (!response.opened) return false;

  state_.setThreadId(response.thread.threadId);
  state_.setAgentId(response.focusedAgentId);
  state_.setThreadTitle(response.thread.title);

  loadTranscript();
  return true;
}

bool ActionDispatcher::interruptAgent() {
  auto result = session_.interruptAgent(state_.threadId(), state_.agentId());
  return result.has_value();
}

bool ActionDispatcher::resolvePermission(
    const std::string &requestId,
    firmius::shared::PermissionResponse response) {
  bool ok = session_.resolvePermission(requestId, response);
  if (ok) {
    state_.clearPendingPermission();
  }
  return ok;
}

void ActionDispatcher::loadTranscript() {
  auto threadId = state_.threadId();
  auto agentId = state_.agentId();
  if (threadId.empty()) return;

  auto snapshot = session_.getTranscript(threadId, agentId);
  if (!snapshot.has_value()) return;

  auto lines = TranscriptRenderer::turnsToLines(snapshot->expandedTurns, 80);
  state_.setTranscriptLines(std::move(lines));
}

} // namespace firmius::tui2
