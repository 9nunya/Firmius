#include "ActionDispatcher.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"

#include <filesystem>

namespace firmius::tui2 {

namespace {

bool sameRootLane(const firmius::tui2::AgentState &existing,
                  const firmius::daemon::AgentRuntimeSnapshot &agent) {
  if (!existing.parentId.empty() || !agent.parentAgentId.empty()) {
    return false;
  }
  if (!existing.friendlyName.empty() && !agent.friendlyName.empty() &&
      existing.friendlyName == agent.friendlyName) {
    return true;
  }
  if (!existing.title.empty() && !agent.title.empty() &&
      existing.title == agent.title) {
    return true;
  }
  return !existing.personaName.empty() && !agent.persona.empty() &&
         existing.personaName == agent.persona;
}

void hydrateAgentState(AppState &state,
                       const firmius::daemon::AgentRuntimeSnapshot &agent,
                       bool primary) {
  std::string duplicateKey;
  if (agent.parentAgentId.empty()) {
    for (const auto &existing : state.agentList()) {
      if (existing.agentId != agent.agentId && sameRootLane(existing, agent)) {
        duplicateKey = existing.agentId;
        break;
      }
    }
  }

  if (!duplicateKey.empty() && duplicateKey != agent.agentId) {
    state.renameAgent(duplicateKey, agent.agentId);
  }
  auto &agentState = state.getOrCreateAgent(agent.agentId);
  agentState.agentId = agent.agentId;
  agentState.parentId = agent.parentAgentId;
  agentState.personaName = agent.persona;
  agentState.friendlyName = agent.friendlyName;
  agentState.title = agent.title;
  agentState.providerId = agent.providerId;
  agentState.modelId = agent.modelId;
  agentState.contextWindowTokens = agent.contextWindowTokens;
  agentState.contextUsedTokens = agent.contextUsedTokens;
  agentState.contextSentTokens = agent.contextSentTokens;
  agentState.status = agent.status;
  agentState.running = agent.running;
  agentState.booting = agent.booting;
  if (primary) {
    state.setPrimaryAgentId(agent.agentId);
    state.focusAgent(agent.agentId);
  }
}

void hydrateThreadAgents(DaemonSession &session, AppState &state,
                         const std::string &threadId,
                         const std::string &focusedAgentId) {
  auto tree = session.listAgents(threadId);
  for (const auto &agent : tree.agents) {
    hydrateAgentState(state, agent, agent.agentId == focusedAgentId);
  }
}

} // namespace

ActionDispatcher::ActionDispatcher(DaemonSession &session, AppState &state)
    : session_(session), state_(state) {}

bool ActionDispatcher::sendMessage(const std::string &text) {
  if (text.empty()) return false;

  try {
    auto threadId = state_.threadId();
    auto agentId = state_.focusedAgentId();

    if (threadId.empty()) {
      if (!createThread()) return false;
      threadId = state_.threadId();
      agentId = state_.focusedAgentId();
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

  state_.prepareForThreadLoad();
  state_.setThreadId(response.thread.threadId);
  state_.setAgentId(response.focusedAgentId);
  state_.setThreadTitle(response.thread.title);
  state_.setLiveMessage("");
  // Items are managed by AppState — no setTranscriptLines needed

  session_.openThread(response.thread.threadId);
  hydrateThreadAgents(session_, state_, response.thread.threadId,
                      response.focusedAgentId);
  state_.setHookState(session_.client().hookState(
      firmius::daemon::HooksStateRequest{response.thread.threadId,
                                         response.focusedAgentId, "", 24}));
  if (!response.focusedAgentId.empty()) {
    auto agent = session_.getAgent(response.thread.threadId, response.focusedAgentId);
    if (agent) {
      state_.setAgentPurpose(agent->persona);
      if (!agent->modelId.empty()) {
        std::string label = agent->providerId.empty() ? agent->modelId
                                                      : agent->providerId + "/" + agent->modelId;
        state_.setModelLabel(label);
      }
      state_.setAgentContextUsage(ContextUsage{
          agent->contextWindowTokens,
          agent->contextUsedTokens,
          agent->contextSentTokens,
      });
    }
  }
  return true;
}

bool ActionDispatcher::openThread(const std::string &threadId) {
  auto response = session_.openThread(threadId);
  if (!response.opened) return false;

  state_.prepareForThreadLoad();
  state_.setThreadId(response.thread.threadId);
  state_.setAgentId(response.focusedAgentId);
  state_.setThreadTitle(response.thread.title);
  state_.setLiveMessage("");
  hydrateThreadAgents(session_, state_, response.thread.threadId,
                      response.focusedAgentId);
  state_.setHookState(session_.client().hookState(
      firmius::daemon::HooksStateRequest{response.thread.threadId,
                                         response.focusedAgentId, "", 24}));

  if (!response.focusedAgentId.empty()) {
    auto agent = session_.getAgent(response.thread.threadId, response.focusedAgentId);
    if (agent) {
      state_.setAgentPurpose(agent->persona);
      if (!agent->modelId.empty()) {
        std::string label = agent->modelId;
        if (!agent->providerId.empty()) label = agent->providerId + "/" + agent->modelId;
        state_.setModelLabel(label);
      }
      state_.setAgentContextUsage(ContextUsage{
          agent->contextWindowTokens,
          agent->contextUsedTokens,
          agent->contextSentTokens,
      });
      if (agent->contextWindowTokens > 0) {
        const uint32_t displayTokens = agent->contextUsedTokens > 0
                                           ? agent->contextUsedTokens
                                           : agent->contextSentTokens;
        state_.setAgentContextWindow(
            std::to_string(displayTokens / 1000) + "k/" +
            std::to_string(agent->contextWindowTokens / 1000) + "k");
      }
    }
    auto todo = session_.getAgentTodo(response.thread.threadId, response.focusedAgentId);
    if (todo) {
      state_.setAgentTodos(response.focusedAgentId, todo->items);
    }
  }

  loadTranscriptForAgent(response.focusedAgentId, false);
  return true;
}

bool ActionDispatcher::interruptAgent(bool flushQueuedMessages) {
  try {
    if (flushQueuedMessages) {
      auto flushed = session_.abortAndFlushQueuedMessages(state_.threadId(),
                                                          state_.focusedAgentId());
      if (!flushed.has_value()) {
        session_.interruptAgent(state_.threadId(), state_.focusedAgentId());
      }
    } else {
      session_.interruptAgent(state_.threadId(), state_.focusedAgentId());
    }
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
  loadTranscriptForAgent(state_.agentId(), false);
}

void ActionDispatcher::loadTranscriptForAgent(const std::string &agentId,
                                              bool replace) {
  auto threadId = state_.threadId();
  if (threadId.empty() || agentId.empty()) return;

  if (replace) {
    state_.clearTranscriptItems();
  }

  auto snapshot = session_.getTranscript(threadId, agentId);
  if (!snapshot.has_value()) return;

  // Convert AgentTurn messages into items
  for (const auto& turn : snapshot->expandedTurns) {
    for (const auto& msg : turn.messages) {
      if (msg.role == firmius::shared::Role::System) continue;

      for (const auto& part : msg.content) {
        if (const auto* text = std::get_if<firmius::shared::TextContent>(&part)) {
          if (msg.role == firmius::shared::Role::User) {
            state_.addItem(std::make_unique<UserMessageItem>(text->text, agentId));
          } else {
            auto item = std::make_unique<AgentTextItem>();
            item->setAgentId(agentId);
            item->appendDelta(text->text);
            item->finalize();
            state_.addItem(std::move(item));
          }
        } else if (const auto* thinking = std::get_if<firmius::shared::ThinkingContent>(&part)) {
          auto item = std::make_unique<AgentThinkingItem>();
          item->setAgentId(agentId);
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

  for (const auto& tool : session_.listToolCalls(threadId, agentId)) {
    auto* item = state_.findToolCallById(tool.toolCallId);
    if (!item) {
      auto created =
          std::make_unique<ToolCallItem>(tool.toolCallId, tool.toolName, agentId);
      created->setAppState(&state_);
      if (!tool.toolArgsJson.empty()) {
        created->setArgs(tool.toolArgsJson);
      }
      item = created.get();
      state_.addItem(std::move(created));
    }

    if (!tool.toolArgsJson.empty()) {
      item->setArgs(tool.toolArgsJson);
    }
    if (!tool.subagentId.empty()) {
      item->setSubagentId(tool.subagentId);
      if (auto* tc = state_.findToolCallState(tool.toolCallId)) {
        tc->subagentId = tool.subagentId;
      }
    }

    if (tool.status == "called") {
      item->setPhase(ToolPhase::Called);
      item->setLive(true);
    } else if (tool.success.has_value()) {
      item->setResult(*tool.success, tool.resultJson);
    }
  }

  const auto subagents = session_.client().subagentActivity(
      firmius::daemon::SubagentsActivityRequest{threadId, agentId});
  for (const auto& activity : subagents.activities) {
    if (activity.childAgentId.empty()) continue;
    const int take =
        std::min<int>(2, static_cast<int>(activity.activityLog.size()));
    for (int i = 0; i < take; ++i) {
      const auto& entry =
          activity.activityLog[activity.activityLog.size() - take + i];
      state_.upsertAgentActivity(
          activity.childAgentId,
          "reload:" + activity.childAgentId + ":" + std::to_string(i),
          entry.summary);
    }
  }

  for (const auto& queued : state_.queuedUserMessagesForAgent(agentId)) {
    if (state_.findUserMessageById(queued.messageId) != nullptr) {
      continue;
    }
    state_.addItem(std::make_unique<UserMessageItem>(
        queued.text, queued.agentId, queued.messageId, true));
  }
  state_.markDirtyPublic();
}

bool ActionDispatcher::executeWorkflow(const std::string &workflowId,
                                        const std::vector<std::string> &args) {
  if (workflowId.empty()) return false;

  // Ensure a thread exists and is focused (workflows need an active context).
  if (state_.threadId().empty()) {
    if (!createThread()) return false;
  }

  try {
    return session_.executeWorkflow(workflowId, args);
  } catch (const std::exception &e) {
    state_.addItem(std::make_unique<ErrorMessageItem>(
        std::string("Workflow failed: ") + e.what()));
    state_.markDirtyPublic();
    return false;
  }
}

} // namespace firmius::tui2
