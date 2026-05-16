#include "ActionDispatcher.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"

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
    state_.addItem(std::make_unique<SystemNoticeItem>(
        std::string("Send failed: ") + e.what()));
    state_.markDirtyPublic();
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
  // Items are managed by AppState — no setTranscriptLines needed

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
    state_.addItem(std::make_unique<SystemNoticeItem>(
        std::string("Interrupt failed: ") + e.what()));
  }
  state_.markDirtyPublic();
  return true;
}

bool ActionDispatcher::resolvePermission(
    const std::string &requestId,
    firmius::shared::PermissionResponse response) {
  bool ok = session_.resolvePermission(requestId, response);
  if (ok) {
    state_.popPendingPermission(requestId);
  }
  return ok;
}

void ActionDispatcher::loadTranscript() {
  auto threadId = state_.threadId();
  auto agentId = state_.agentId();
  if (threadId.empty()) return;

  auto snapshot = session_.getTranscript(threadId, agentId);
  if (!snapshot.has_value()) return;

  // Convert AgentTurn messages into items
  for (const auto& turn : snapshot->expandedTurns) {
    for (const auto& msg : turn.messages) {
      if (msg.role == firmius::shared::Role::System) continue;

      for (const auto& part : msg.content) {
        if (const auto* text = std::get_if<firmius::shared::TextContent>(&part)) {
          if (msg.role == firmius::shared::Role::User) {
            state_.addItem(std::make_unique<UserMessageItem>(text->text));
          } else {
            auto item = std::make_unique<AgentTextItem>();
            item->appendDelta(text->text);
            item->finalize();
            state_.addItem(std::move(item));
          }
        } else if (const auto* thinking = std::get_if<firmius::shared::ThinkingContent>(&part)) {
          auto item = std::make_unique<AgentThinkingItem>();
          item->appendDelta(thinking->thinking);
          item->finalize();
          state_.addItem(std::move(item));
        } else if (const auto* toolCall = std::get_if<firmius::shared::ToolCallContent>(&part)) {
          auto item = std::make_unique<ToolCallItem>(toolCall->id, toolCall->name, agentId);
          if (!toolCall->args.empty()) {
            item->setArgs(toolCall->args);
          }
          state_.addItem(std::move(item));
        } else if (const auto* toolResult = std::get_if<firmius::shared::ToolResultContent>(&part)) {
          auto* tc = state_.findToolCallById(toolResult->toolCallId);
          if (tc) {
            tc->setResult(toolResult->success, toolResult->result);
          }
        } else if (const auto* error = std::get_if<firmius::shared::ErrorContent>(&part)) {
          state_.addItem(std::make_unique<ErrorMessageItem>(
              error->errorName + ": " + error->description));
        } else if (const auto* notice = std::get_if<firmius::shared::NoticeContent>(&part)) {
          state_.addItem(std::make_unique<SystemNoticeItem>(
              notice->title + ": " + notice->message));
        }
      }
    }
  }
  state_.markDirtyPublic();
}

} // namespace firmius::tui2
