#include "ActionDispatcher.hpp"
#include "TranscriptRenderer.hpp"

#include <filesystem>

namespace firmius::tui2 {

ActionDispatcher::ActionDispatcher(DaemonSession &session, AppState &state)
    : session_(session), state_(state) {}

bool ActionDispatcher::sendMessage(const std::string &text) {
  if (text.empty()) return false;

  try {
    auto threadId = state_.threadId();
    auto agentId = state_.agentId();

    if (threadId.empty()) {
      if (!createThread()) return false;
      threadId = state_.threadId();
      agentId = state_.agentId();
    }

    auto response = session_.send(threadId, agentId, text);
    return response.accepted;
  } catch (const std::exception& e) {
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::Notice;
    line.text = std::string("Send failed: ") + e.what();
    state_.appendTranscriptLine(std::move(line));
    state_.finalizeStreamingLine();
    state_.setAgentStatus(firmius::shared::AgentStatus::Idle);
    return false;
  }
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

  session_.openThread(response.thread.threadId);
  return true;
}

bool ActionDispatcher::openThread(const std::string &threadId) {
  auto response = session_.openThread(threadId);
  if (!response.opened) return false;

  state_.setThreadId(response.thread.threadId);
  state_.setAgentId(response.focusedAgentId);
  state_.setThreadTitle(response.thread.title);

  if (!response.focusedAgentId.empty()) {
    auto agent = session_.getAgent(response.thread.threadId, response.focusedAgentId);
    if (agent) {
      state_.setAgentPurpose(agent->persona);
      if (!agent->modelId.empty()) {
        std::string label = agent->modelId;
        if (!agent->providerId.empty()) label = agent->providerId + "/" + agent->modelId;
        state_.setModelLabel(label);
      }
      if (agent->maxTokens > 0) {
        state_.setAgentContextWindow(std::to_string(agent->maxTokens / 1000) + "k ctx");
      }
    }
  }

  loadTranscript();
  return true;
}

bool ActionDispatcher::interruptAgent() {
  try {
    session_.interruptAgent(state_.threadId(), state_.agentId());
  } catch (const std::exception& e) {
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::Notice;
    line.text = std::string("Interrupt failed: ") + e.what();
    state_.appendTranscriptLine(std::move(line));
  }
  state_.finalizeStreamingLine();
  state_.setAgentStatus(firmius::shared::AgentStatus::Idle);
  return true;
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
