#ifndef FIRMIUS_STREAM_STATE_MANAGER_HPP
#define FIRMIUS_STREAM_STATE_MANAGER_HPP

#include "Events.hpp"
#include "utils/ToolView.hpp"
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace firmius::tui {

using firmius::shared::ToolCallView;

struct TimelineEntry {
  enum class Kind { ToolCall, Error };
  Kind kind;
  std::string id;
  std::string message;
  std::string agentId;
};

struct StreamState {
  std::string thinking;
  std::string text;
  std::string compaction_thinking;
  std::string compaction_text;
  bool provider_waiting = false;
  std::chrono::steady_clock::time_point thinking_start{};
  bool is_thinking = false;
};

class StreamStateManager {
public:
  void handleAgentThinking(const shared::AgentThinking &e);
  void handleAgentText(const shared::AgentText &e);
  void handleAgentTurnCompleted(const shared::AgentTurnCompleted &e);
  void handleAgentProviderWaiting(const shared::AgentProviderWaiting &e);
  void handleAgentToolCallChunk(const shared::AgentToolCallChunk &e);
  void handleAgentToolCall(const shared::AgentToolCall &e);
  void handleAgentCompactionThinking(const shared::AgentCompactionThinking &e);
  void handleAgentCompactionText(const shared::AgentCompactionText &e);
  void handleContextCompacted(const shared::ContextCompacted &e);
  void handleAgentProcessSpawned(const shared::AgentProcessSpawned &e);
  void handleAgentProcessOutput(const shared::AgentProcessOutput &e);
  void handleAgentCompleted(const shared::AgentCompleted &e);
  void handleAgentSpawned(const shared::AgentSpawned &e,
                          const std::string &focused_agent_id);
  void handleAgentRetrying(const shared::AgentRetrying &e);
  void handleAgentRetryFailed(const shared::AgentRetryFailed &e);
  void handleAgentAccountSwitched(const shared::AgentAccountSwitched &e);
  void handleMessageQueued(const shared::MessageQueued &e);
  void handleMessageDequeued(const shared::MessageDequeued &e);
  void handleThreadChanged();

  const StreamState *getStream(const std::string &agentId) const;
  const std::vector<TimelineEntry> &getTimeline() const;
  const std::unordered_map<std::string, std::shared_ptr<ToolCallView>> &
  getToolCalls() const;
  std::shared_ptr<ToolCallView>
  getToolView(const std::string &toolCallId) const;
  const std::string &getRetryStatus() const;
  const std::vector<std::string> &getAccountSwaps() const;
  const std::vector<std::pair<std::string, std::string>> &
  getQueuedMessages() const;

private:
  void pushThinkingDuration(const std::string &agentId, float seconds);
  void pushTokenUsage(const std::string &agentId,
                      const shared::AgentMetrics &metrics);
  void clearRetryStatus();

  std::unordered_map<std::string, StreamState> streams_;
  std::unordered_map<std::string, std::shared_ptr<ToolCallView>> tool_calls_;
  std::vector<TimelineEntry> timeline_;

  std::unordered_map<std::string, std::string> process_outputs_;
  std::unordered_map<std::string, std::string> subagent_outputs_;
  std::unordered_map<std::string, std::string> process_to_toolcall_;
  std::unordered_map<std::string, std::string> current_process_for_agent_;
  std::unordered_map<std::string, std::string> current_subagent_for_agent_;
  std::unordered_map<std::string, std::string> subagent_to_parent_tool_;
  std::unordered_map<std::string, std::string> agent_provider_model_;

  std::string retry_status_;
  std::vector<std::string> account_swaps_;
  std::vector<std::pair<std::string, std::string>> queued_messages_; // id, text
};

} // namespace firmius::tui

#endif
