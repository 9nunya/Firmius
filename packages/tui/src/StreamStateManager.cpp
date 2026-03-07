#include "StreamStateManager.hpp"
#include "components/ToolBlock.hpp"
#include "components/ToolWindow.hpp"
#include <rapidjson/document.h>

namespace firmius::tui {

void StreamStateManager::handleAgentThinking(const shared::AgentThinking &e) {
  auto &s = streams_[e.agentId];
  s.thinking += e.delta;
  s.provider_waiting = false;
}

void StreamStateManager::handleAgentText(const shared::AgentText &e) {
  auto &s = streams_[e.agentId];
  s.text += e.delta;
  s.provider_waiting = false;
}

void StreamStateManager::handleAgentTurnCompleted(
    const shared::AgentTurnCompleted &e) {
  auto &s = streams_[e.agentId];
  s.thinking.clear();
  s.text.clear();
  s.provider_waiting = false;
  for (auto it = tool_order_.begin(); it != tool_order_.end();) {
    auto it_tool = tool_calls_.find(*it);
    if (it_tool != tool_calls_.end() && it_tool->second &&
        it_tool->second->agentId == e.agentId) {
      tool_calls_.erase(it_tool);
      it = tool_order_.erase(it);
    } else {
      ++it;
    }
  }
}

void StreamStateManager::handleAgentProviderWaiting(
    const shared::AgentProviderWaiting &e) {
  streams_[e.agentId].provider_waiting = true;
}

void StreamStateManager::handleAgentToolCallChunk(
    const shared::AgentToolCallChunk &e) {
  auto &view = tool_calls_[e.toolCallId];
  if (!view) {
    view = std::make_shared<ToolCallView>();
    view->toolCallId = e.toolCallId;
    view->agentId = e.agentId;
    tool_order_.push_back(e.toolCallId);
  }
  view->phase = ToolPhase::Preparing;
  view->name += e.nameDelta;
  view->args += e.argsDelta;
  if (!view->args.empty()) {
    view->phase = ToolPhase::Called;
  }

  // If this agent is a subagent, push summarized tool into parent's log
  auto it_sub = subagent_to_parent_tool_.find(e.agentId);
  if (it_sub != subagent_to_parent_tool_.end()) {
    auto it_parent = tool_calls_.find(it_sub->second);
    if (it_parent != tool_calls_.end() && it_parent->second) {
      std::string summary = SummarizeToolCall(view->name, view->args);
      auto &log = it_parent->second->subagent_tool_log;
      // Only push if this is a new summary (avoid duplicates from streaming)
      if (log.empty() || log.back() != summary) {
        log.push_back(summary);
        if (log.size() > 3)
          log.erase(log.begin());
      }
    }
  }
}

void StreamStateManager::handleAgentToolCall(const shared::AgentToolCall &e) {
  auto &view = tool_calls_[e.toolCallId];
  if (!view) {
    view = std::make_shared<ToolCallView>();
    view->toolCallId = e.toolCallId;
    tool_order_.push_back(e.toolCallId);
  }
  view->agentId = e.agentId;
  if (!e.toolName.empty())
    view->name = e.toolName;
  if (!e.toolArgs.empty())
    view->args = e.toolArgs;
  view->phase = view->args.empty() ? ToolPhase::Preparing : ToolPhase::Called;
}

void StreamStateManager::handleAgentCompactionThinking(
    const shared::AgentCompactionThinking &e) {
  streams_[e.agentId].compaction_thinking += e.delta;
}

void StreamStateManager::handleAgentCompactionText(
    const shared::AgentCompactionText &e) {
  streams_[e.agentId].compaction_text += e.delta;
}

void StreamStateManager::handleContextCompacted(
    const shared::ContextCompacted &e) {
  auto &s = streams_[e.agentId];
  s.compaction_thinking.clear();
  s.compaction_text.clear();
}

void StreamStateManager::handleAgentProcessSpawned(
    const shared::AgentProcessSpawned &e) {
  if (!e.toolCallId.empty()) {
    process_to_toolcall_[e.processId] = e.toolCallId;
  }
}

void StreamStateManager::handleAgentProcessOutput(
    const shared::AgentProcessOutput &e) {
  auto it_pid = process_to_toolcall_.find(e.processId);
  if (it_pid != process_to_toolcall_.end()) {
    auto tid = it_pid->second;
    auto it_tool = tool_calls_.find(tid);
    if (it_tool != tool_calls_.end() && it_tool->second &&
        it_tool->second->phase == ToolPhase::Called) {
      it_tool->second->live_process_output += e.output;
    }
  }
}

void StreamStateManager::handleAgentSpawned(
    const shared::AgentSpawned &e, const std::string &focused_agent_id) {
  for (auto &pair : tool_calls_) {
    if (pair.second && pair.second->agentId == e.parentId &&
        pair.second->phase == ToolPhase::Called) {
      // Track the subagent -> parent tool mapping
      subagent_to_parent_tool_[e.agentId] = pair.first;

      // Set the title on the parent tool view
      if (!e.title.empty()) {
        pair.second->subagent_title = e.title;
      } else {
        pair.second->subagent_title = e.personaName;
      }
      break;
    }
  }
  (void)focused_agent_id;
}

const StreamState *
StreamStateManager::getStream(const std::string &agentId) const {
  auto it = streams_.find(agentId);
  if (it == streams_.end())
    return nullptr;
  return &it->second;
}

const std::vector<std::string> &StreamStateManager::getToolOrder() const {
  return tool_order_;
}

const std::unordered_map<std::string, std::shared_ptr<ToolCallView>> &
StreamStateManager::getToolCalls() const {
  return tool_calls_;
}

} // namespace firmius::tui
