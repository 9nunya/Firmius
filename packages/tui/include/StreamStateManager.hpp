#ifndef FIRMIUS_STREAM_STATE_MANAGER_HPP
#define FIRMIUS_STREAM_STATE_MANAGER_HPP

#include "Events.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::tui {

struct ToolCallView;

struct StreamState {
  std::string thinking;
  std::string text;
  std::string compaction_thinking;
  std::string compaction_text;
  bool provider_waiting = false;
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
  void handleAgentSpawned(const shared::AgentSpawned &e,
                          const std::string &focused_agent_id);

  const StreamState *getStream(const std::string &agentId) const;
  const std::vector<std::string> &getToolOrder() const;
  const std::unordered_map<std::string, std::shared_ptr<ToolCallView>> &
  getToolCalls() const;

private:
  std::unordered_map<std::string, StreamState> streams_;
  std::unordered_map<std::string, std::shared_ptr<ToolCallView>> tool_calls_;
  std::vector<std::string> tool_order_;

  std::unordered_map<std::string, std::string> process_outputs_;
  std::unordered_map<std::string, std::string> subagent_outputs_;
  std::unordered_map<std::string, std::string> process_to_toolcall_;
  std::unordered_map<std::string, std::string>
      current_process_for_agent_; // agentId -> pid
  std::unordered_map<std::string, std::string>
      current_subagent_for_agent_; // agentId -> subagentId
  std::unordered_map<std::string, std::string>
      subagent_to_parent_tool_; // subagentAgentId -> parent toolCallId
};

} // namespace firmius::tui

#endif
