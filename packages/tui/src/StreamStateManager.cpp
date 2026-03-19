#include "StreamStateManager.hpp"
#include "components/ToolBlock.hpp"
#include "utils/ToolSummaries.hpp"
#include "harness/Harness.hpp"
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <rapidjson/document.h>

namespace firmius::tui {

namespace {

struct ParsedProcessResult {
  std::string process_id;
  std::string finish_reason;
  int exit_code = 0;
  bool exit_known = false;
  double duration_ms = 0.0;
};

ParsedProcessResult parseProcessResult(const std::string &result) {
  ParsedProcessResult parsed;
  rapidjson::Document doc;
  doc.Parse(result.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return parsed;
  }

  if (doc.HasMember("process_id") && doc["process_id"].IsString()) {
    parsed.process_id = doc["process_id"].GetString();
  }
  if (doc.HasMember("finish_reason") && doc["finish_reason"].IsString()) {
    parsed.finish_reason = doc["finish_reason"].GetString();
  }
  if (doc.HasMember("exit_code") && doc["exit_code"].IsInt()) {
    parsed.exit_code = doc["exit_code"].GetInt();
    parsed.exit_known = true;
  }
  if (doc.HasMember("duration_ms") && doc["duration_ms"].IsNumber()) {
    parsed.duration_ms = doc["duration_ms"].GetDouble();
  }

  return parsed;
}

} // namespace

static std::string formatDuration(float seconds) {
  if (seconds < 0.1f)
    return "<0.1s";
  int tenths = static_cast<int>(seconds * 10 + 0.5f);
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "s";
}

void StreamStateManager::clearRetryStatus() {
  retry_status_.clear();
  account_swaps_.clear();
}

void StreamStateManager::handleAgentThinking(const shared::AgentThinking &e) {
  auto &s = streams_[e.agentId];
  if (!s.is_thinking) {
    s.thinking_start = std::chrono::steady_clock::now();
    s.is_thinking = true;
  }
  s.compaction_finished = false;
  s.thinking += e.delta;
  appendLiveTimelineDelta(e.agentId, TimelineEntry::Kind::Thinking, e.delta);
  if (!e.delta.empty()) {
    live_quick_clusters_[e.agentId].prose_since_last_tool = true;
  }
  s.provider_waiting = false;
  clearRetryStatus();
}

void StreamStateManager::handleAgentText(const shared::AgentText &e) {
  auto &s = streams_[e.agentId];
  if (s.is_thinking) {
    auto elapsed = std::chrono::steady_clock::now() - s.thinking_start;
    float secs = std::chrono::duration<float>(elapsed).count();
    s.is_thinking = false;
    pushThinkingDuration(e.agentId, secs);
  }
  s.compaction_finished = false;
  s.text += e.delta;
  appendLiveTimelineDelta(e.agentId, TimelineEntry::Kind::Text, e.delta);
  if (!e.delta.empty()) {
    live_quick_clusters_[e.agentId].prose_since_last_tool = true;
  }
  s.provider_waiting = false;
  clearRetryStatus();
}

void StreamStateManager::handleAgentTurnCompleted(
    const shared::AgentTurnCompleted &e) {
  auto &s = streams_[e.agentId];
  s.thinking.clear();
  s.text.clear();
  s.compaction_finished = false;
  s.provider_waiting = false;
  s.is_thinking = false;
  s.has_active_live_entry = false;
  s.active_live_entry_id.clear();
  pushTokenUsage(e.agentId, e.aggregateMetrics);

  // Process tool results from the turn to determine success/failure
  std::unordered_map<std::string, std::pair<bool, std::string>> toolResultMap;
  for (const auto &msg : e.turn.messages) {
    if (msg.role == shared::Role::ToolResult) {
      for (const auto &content : msg.content) {
        if (auto *trc = std::get_if<shared::ToolResultContent>(&content)) {
          toolResultMap[trc->toolCallId] = {trc->success, trc->result};
        }
      }
    }
  }

  // Mark all pending tool calls for this agent as finished with proper success state
  for (auto &[toolId, view] : tool_calls_) {
    if (view && view->agentId == e.agentId &&
        view->name != "summon_subagent" &&
        view->phase != ToolPhase::Finished &&
        view->phase != ToolPhase::Error) {
      auto it = toolResultMap.find(toolId);
      if (it != toolResultMap.end()) {
        applyToolResult(view, it->second.first, it->second.second);
      }
    }
  }

  // Mark all pending subagent tool log entries as finished and update summaries
  for (auto &[toolId, view] : tool_calls_) {
    if (view && view->name == "summon_subagent") {
      for (auto &entry : view->subagent_tool_log) {
        if (entry.phase != ToolPhase::Finished && !entry.name.empty()) {
          // Regenerate summary with stored name/args
          entry.summary = shared::SummarizeToolCall(
              entry.name, entry.args, ToolPhase::Finished);
          entry.phase = ToolPhase::Finished;
        }
      }
    }
  }

  for (auto it = tool_calls_.begin(); it != tool_calls_.end();) {
    if (it->second && it->second->agentId == e.agentId &&
        it->second->name != "summon_subagent" &&
        it->second->phase != ToolPhase::BackgroundRunning &&
        it->second->phase != ToolPhase::Called &&
        it->second->phase != ToolPhase::Preparing) {
      it = tool_calls_.erase(it);
    } else {
      ++it;
    }
  }
  timeline_.erase(
      std::remove_if(
          timeline_.begin(), timeline_.end(), [&](const TimelineEntry &entry) {
            if (entry.agentId != e.agentId) {
              return false;
            }
            if (entry.kind == TimelineEntry::Kind::Thinking ||
                entry.kind == TimelineEntry::Kind::Text) {
              return true;
            }
            if (entry.kind != TimelineEntry::Kind::ToolCall) {
              return false;
            }
            auto it_tool = tool_calls_.find(entry.id);
            if (it_tool != tool_calls_.end() && it_tool->second) {
              auto phase = it_tool->second->phase;
              if (phase == ToolPhase::BackgroundRunning ||
                  phase == ToolPhase::Called || phase == ToolPhase::Preparing) {
                return false;
              }
            }
            return true;
          }),
      timeline_.end());

  for (auto it = tool_call_cluster_ids_.begin();
       it != tool_call_cluster_ids_.end();) {
    if (tool_calls_.count(it->first) == 0) {
      it = tool_call_cluster_ids_.erase(it);
    } else {
      ++it;
    }
  }
  live_quick_clusters_[e.agentId] = {};
}

void StreamStateManager::handleAgentProviderWaiting(
    const shared::AgentProviderWaiting &e) {
  streams_[e.agentId].provider_waiting = true;
  clearRetryStatus();
}

void StreamStateManager::handleAgentToolCallChunk(
    const shared::AgentToolCallChunk &e) {
  auto it_stream = streams_.find(e.agentId);
  if (it_stream != streams_.end() && it_stream->second.is_thinking) {
    auto elapsed =
        std::chrono::steady_clock::now() - it_stream->second.thinking_start;
    float secs = std::chrono::duration<float>(elapsed).count();
    it_stream->second.is_thinking = false;
    pushThinkingDuration(e.agentId, secs);
  }
  clearActiveLiveEntry(e.agentId);

  auto &view = tool_calls_[e.toolCallId];
  if (!view) {
    view = std::make_shared<ToolCallView>();
    view->toolCallId = e.toolCallId;
    view->agentId = e.agentId;
    timeline_.push_back(
        {TimelineEntry::Kind::ToolCall, e.toolCallId, "", e.agentId});
  }
  assignToolCallClusterId(e.agentId, e.toolCallId);
  view->phase = ToolPhase::Preparing;
  if (!e.nameDelta.empty()) {
    view->name += e.nameDelta;
  }
  view->args += e.argsDelta;
  if (!view->args.empty()) {
    view->phase = ToolPhase::Called;
  }

  auto it_sub = subagent_to_parent_tool_.find(e.agentId);
  if (it_sub != subagent_to_parent_tool_.end()) {
    auto it_parent = tool_calls_.find(it_sub->second);
    if (it_parent != tool_calls_.end() && it_parent->second) {
      auto phase = view->phase;
      std::string summary =
          firmius::shared::SummarizeToolCall(view->name, view->args, phase);
      auto &log = it_parent->second->subagent_tool_log;
      if (!summary.empty()) {
        // Search for existing entry with this toolCallId (for parallel subagents)
        auto it_entry = std::find_if(log.begin(), log.end(),
            [&e](const shared::SubagentToolLogEntry &entry) {
              return entry.toolCallId == e.toolCallId;
            });

        if (it_entry != log.end()) {
          // Update existing entry
          it_entry->summary = summary;
          it_entry->phase = phase;
          it_entry->name = view->name;
          it_entry->args = view->args;
        } else {
          // Create new entry
          shared::SubagentToolLogEntry entry;
          entry.summary = summary;
          entry.phase = phase;
          entry.toolCallId = e.toolCallId;
          entry.name = view->name;
          entry.args = view->args;
          log.push_back(entry);
          while (log.size() > 8)
            log.erase(log.begin());
        }
      }
    }
  }
}

void StreamStateManager::handleAgentToolCall(const shared::AgentToolCall &e) {
  clearActiveLiveEntry(e.agentId);
  auto &view = tool_calls_[e.toolCallId];
  if (!view) {
    view = std::make_shared<ToolCallView>();
    view->toolCallId = e.toolCallId;
    timeline_.push_back(
        {TimelineEntry::Kind::ToolCall, e.toolCallId, "", e.agentId});
  }
  assignToolCallClusterId(e.agentId, e.toolCallId);
  view->agentId = e.agentId;
  if (!e.toolName.empty())
    view->name = e.toolName;
  if (!e.toolArgs.empty())
    view->args = e.toolArgs;
  view->phase = view->args.empty() ? ToolPhase::Preparing : ToolPhase::Called;

  auto it_sub = subagent_to_parent_tool_.find(e.agentId);
  if (it_sub != subagent_to_parent_tool_.end()) {
    auto it_parent = tool_calls_.find(it_sub->second);
    if (it_parent != tool_calls_.end() && it_parent->second) {
      std::string summary = firmius::shared::SummarizeToolCall(
          view->name, view->args, view->phase);
      auto &log = it_parent->second->subagent_tool_log;
      if (!summary.empty()) {
        // Search for existing entry with this toolCallId (for parallel subagents)
        auto it_entry = std::find_if(log.begin(), log.end(),
            [&e](const shared::SubagentToolLogEntry &entry) {
              return entry.toolCallId == e.toolCallId;
            });

        if (it_entry != log.end()) {
          // Update existing entry
          it_entry->summary = summary;
          it_entry->phase = view->phase;
          it_entry->name = view->name;
          it_entry->args = view->args;
        } else {
          // Create new entry
          shared::SubagentToolLogEntry entry;
          entry.summary = summary;
          entry.phase = view->phase;
          entry.toolCallId = e.toolCallId;
          entry.name = view->name;
          entry.args = view->args;
          log.push_back(entry);
          while (log.size() > 8)
            log.erase(log.begin());
        }
      }
    }
  }

  // Handle subagent_wait linking
  if (view->name == "subagent_wait" && !view->args.empty()) {
    rapidjson::Document doc;
    doc.Parse(view->args.c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      if (doc.HasMember("agent_id") && doc["agent_id"].IsString()) {
        std::string subId = doc["agent_id"].GetString();
        view->subagent_id = subId;
        auto it_title = agent_titles_.find(subId);
        if (it_title != agent_titles_.end()) {
          view->subagent_title = it_title->second;
        }
      }
    }
  }
}

void StreamStateManager::handleAgentCompacting(
    const shared::AgentCompacting &e) {
  auto &s = streams_[e.agentId];
  s.compaction_active = true;
  s.compaction_finished = false;
  s.compaction_thinking.clear();
  s.compaction_text.clear();
  (void)e;
}

void StreamStateManager::handleAgentCompactionThinking(
    const shared::AgentCompactionThinking &e) {
  auto &s = streams_[e.agentId];
  s.compaction_active = true;
  s.compaction_finished = false;
  s.compaction_thinking += e.delta;
}

void StreamStateManager::handleAgentCompactionText(
    const shared::AgentCompactionText &e) {
  auto &s = streams_[e.agentId];
  s.compaction_active = true;
  s.compaction_finished = false;
  s.compaction_text += e.delta;
}

void StreamStateManager::handleContextCompacted(
    const shared::ContextCompacted &e) {
  auto &s = streams_[e.agentId];
  s.compaction_active = false;
  s.compaction_finished = true;
  s.compaction_thinking.clear();
  s.compaction_text.clear();
}

void StreamStateManager::handleAgentProcessSpawned(
    const shared::AgentProcessSpawned &e) {
  if (!e.toolCallId.empty()) {
    process_to_toolcall_[e.processId] = e.toolCallId;
  }
  process_to_agent_[e.processId] = e.agentId;
  process_background_state_[e.processId] = false;
  process_finished_state_[e.processId] = false;
}

void StreamStateManager::handleAgentProcessOutput(
    const shared::AgentProcessOutput &e) {
  auto it_pid = process_to_toolcall_.find(e.processId);
  if (it_pid != process_to_toolcall_.end()) {
    auto tid = it_pid->second;
    auto it_tool = tool_calls_.find(tid);
    if (it_tool != tool_calls_.end() && it_tool->second) {
      auto &view = it_tool->second;
      if (!e.output.empty() &&
          (view->phase == ToolPhase::Called ||
           view->phase == ToolPhase::BackgroundRunning)) {
        view->live_process_output += e.output;
      }
      if (view->process_id.empty()) {
        view->process_id = e.processId;
      }
      if (e.finished) {
        view->process_exit_known = true;
        view->process_exit_code = e.exitCode;
        view->process_duration_ms = e.durationMs;
        if (view->phase == ToolPhase::BackgroundRunning) {
          view->phase = ToolPhase::Finished;
          view->success = (e.exitCode == 0);
        }
        process_finished_state_[e.processId] = true;
      }
    }
  }
  if (e.finished) {
    process_finished_state_[e.processId] = true;
  }
}

void StreamStateManager::handleAgentSpawned(
    const shared::AgentSpawned &e, const std::string &focused_agent_id) {
  if (!e.providerId.empty() || !e.modelId.empty()) {
    agent_provider_model_[e.agentId] = e.providerId + "/" + e.modelId;
  }
  if (!e.title.empty()) {
    agent_titles_[e.agentId] = e.title;
  }

  // Link spawned agent to parent tool call
  for (auto &pair : tool_calls_) {
    if (!pair.second || pair.second->agentId != e.parentId)
      continue;
    if (pair.second->name != "summon_subagent")
      continue;

    // Check if this tool call is likely the one that spawned this agent
    // We can use the agentId if the tool call reported it (async mode)
    // or check name/slug matches.
    bool id_match = (!pair.second->subagent_id.empty() &&
                     pair.second->subagent_id == e.agentId);
    bool name_match = (!pair.second->subagent_slug.empty() &&
                       pair.second->subagent_slug == e.friendlyName);

    // If we haven't linked a subagent_id yet, and it's a name match, take it.
    if (pair.second->subagent_id.empty() || id_match || name_match) {
      subagent_to_parent_tool_[e.agentId] = pair.first;
      pair.second->subagent_running = true;
      pair.second->subagent_id = e.agentId;
      if (!e.title.empty()) {
        pair.second->subagent_title = e.title;
      }
      if (!e.friendlyName.empty())
        pair.second->subagent_slug = e.friendlyName;
      break;
    }
  }
  (void)focused_agent_id;
}

void StreamStateManager::pushThinkingDuration(const std::string &agentId,
                                              float seconds) {
  auto it_sub = subagent_to_parent_tool_.find(agentId);
  if (it_sub == subagent_to_parent_tool_.end())
    return;
  auto it_parent = tool_calls_.find(it_sub->second);
  if (it_parent == tool_calls_.end() || !it_parent->second)
    return;

  std::string label = "Thought for " + formatDuration(seconds);
  auto &log = it_parent->second->subagent_tool_log;
  shared::SubagentToolLogEntry entry;
  entry.summary = label;
  entry.phase = shared::ToolPhase::Finished;
  entry.toolCallId = "";
  log.push_back(entry);
  while (log.size() > 8)
    log.erase(log.begin());
}

void StreamStateManager::pushTokenUsage(const std::string &,
                                        const shared::AgentMetrics &) {}

void StreamStateManager::handleAgentCompleted(const shared::AgentCompleted &e) {
  auto it_sub = subagent_to_parent_tool_.find(e.agentId);
  if (it_sub != subagent_to_parent_tool_.end()) {
    auto it_parent = tool_calls_.find(it_sub->second);
    if (it_parent != tool_calls_.end() && it_parent->second) {
      shared::SubagentToolLogEntry entry;
      entry.summary = "Done";
      entry.phase = shared::ToolPhase::Finished;
      entry.toolCallId = "";
      it_parent->second->subagent_tool_log.push_back(entry);
      it_parent->second->subagent_running = false;
      it_parent->second->phase = ToolPhase::Finished; // Mark tool as finished when subagent completes
    }
  }
}

void StreamStateManager::handleAgentInterrupted(
    const shared::AgentInterrupted &e) {
  auto &s = streams_[e.agentId];
  s.provider_waiting = false;
  s.is_thinking = false;
  clearActiveLiveEntry(e.agentId);
  clearRetryStatus();
}

const StreamState *
StreamStateManager::getStream(const std::string &agentId) const {
  auto it = streams_.find(agentId);
  if (it == streams_.end())
    return nullptr;
  return &it->second;
}

const std::vector<TimelineEntry> &StreamStateManager::getTimeline() const {
  return timeline_;
}

const std::unordered_map<std::string, std::shared_ptr<ToolCallView>> &
StreamStateManager::getToolCalls() const {
  return tool_calls_;
}

std::shared_ptr<ToolCallView>
StreamStateManager::getToolView(const std::string &toolCallId) const {
  auto it = tool_calls_.find(toolCallId);
  if (it != tool_calls_.end())
    return it->second;
  return nullptr;
}

std::string StreamStateManager::getAgentTitle(
    const std::string &agentId) const {
  auto it = agent_titles_.find(agentId);
  if (it != agent_titles_.end())
    return it->second;
  return "";
}

ProcessCounts StreamStateManager::getProcessCounts(
    const std::string &agentId) const {
  ProcessCounts counts;
  for (const auto &[processId, owner] : process_to_agent_) {
    if (owner != agentId) {
      continue;
    }
    auto it_finished = process_finished_state_.find(processId);
    if (it_finished != process_finished_state_.end() && it_finished->second) {
      continue;
    }
    auto it_background = process_background_state_.find(processId);
    if (it_background != process_background_state_.end() &&
        it_background->second) {
      ++counts.background;
    } else {
      ++counts.live;
    }
  }
  return counts;
}

// Transient error rendering is disabled; errors are now rendered
// persistently via ChatWindow using AgentHistory ErrorContent.

void StreamStateManager::handleAgentRetrying(const shared::AgentRetrying &e) {
  retry_status_ = "Retrying (" + std::to_string(e.attempt) + "/" +
                  std::to_string(e.maxAttempts) + ", HTTP " +
                  std::to_string(e.httpStatus) + ", " + e.reason + ", ~" +
                  std::to_string(e.delayMs / 1000) + "s)";
  if (!e.accountLocator.empty()) {
    retry_status_ += " [Account: " + e.accountLocator + "]";
  }
}

void StreamStateManager::handleAgentAccountSwitched(
    const shared::AgentAccountSwitched &e) {
  account_swaps_.push_back("[Account Switch] -> " + e.accountLocator);
}

void StreamStateManager::handleAgentRetryFailed(
    const shared::AgentRetryFailed &) {
  clearRetryStatus();
  // Transient error rendering is disabled.
}

const std::string &StreamStateManager::getRetryStatus() const {
  return retry_status_;
}

const std::vector<std::string> &StreamStateManager::getAccountSwaps() const {
  return account_swaps_;
}

void StreamStateManager::handleMessageQueued(const shared::MessageQueued &e) {
  queued_messages_.emplace_back(e.messageId, e.text);
}

void StreamStateManager::handleMessageDequeued(
    const shared::MessageDequeued &e) {
  queued_messages_.erase(std::remove_if(queued_messages_.begin(),
                                        queued_messages_.end(),
                                        [&](const auto &pair) {
                                          return pair.first == e.messageId;
                                        }),
                         queued_messages_.end());
}

void StreamStateManager::handleThreadChanged() {
  queued_messages_.clear();
  // Clear live tool calls - they will be rebuilt from history
  tool_calls_.clear();
  timeline_.clear();
  subagent_to_parent_tool_.clear();
  streams_.clear();
  process_to_toolcall_.clear();
  process_to_agent_.clear();
  process_background_state_.clear();
  process_finished_state_.clear();
  live_quick_clusters_.clear();
  tool_call_cluster_ids_.clear();
}

void StreamStateManager::rebuildToolCallsFromHistory(
    const std::string &agentId, const shared::AgentHistory *history,
    const std::string &threadId, bool populate_subagent_log) {
  if (!history)
    return;

  // First pass: collect all tool results to know success/failure.
  // Historical turns may embed ToolResultContent under different message roles,
  // so inspect every message part rather than relying on msg.role.
  std::unordered_map<std::string, std::pair<bool, std::string>> toolResults;
  for (const auto &turn : history->turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &content : msg.content) {
        if (auto *tr = std::get_if<shared::ToolResultContent>(&content)) {
          toolResults[tr->toolCallId] = {tr->success, tr->result};
        }
      }
    }
  }

  // Second pass: extract tool calls and set state based on results
  // For historical data, assume success unless we have explicit error
  for (const auto &turn : history->turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == shared::Role::Assistant) {
        for (const auto &content : msg.content) {
          if (auto *tc = std::get_if<shared::ToolCallContent>(&content)) {
            auto &view = tool_calls_[tc->id];
            if (!view) {
              view = std::make_shared<ToolCallView>();
              view->toolCallId = tc->id;
              view->agentId = agentId;
              view->name = tc->name;
              view->args = tc->args;
              view->subagent_running = false;

              // Check if we have a result for this tool call
              auto it = toolResults.find(tc->id);
              if (it != toolResults.end()) {
                // We have explicit result
                applyToolResult(view, it->second.first, it->second.second);
              } else {
                // No explicit result - for historical data, assume success
                // This handles summon_subagent and other tools that may not have
                // explicit ToolResultContent but completed successfully
                view->success = true;
                view->phase = ToolPhase::Finished;
              }
            }
          }
        }
      }
    }
  }

  // Third pass: populate subagent_tool_log for summon_subagent tool calls
  // by analyzing the history of spawned subagents (only if requested)
  if (!populate_subagent_log)
    return;
    
  for (auto &[toolCallId, view] : tool_calls_) {
    if (!view || view->name != "summon_subagent" || view->args.empty())
      continue;

    // Parse args to extract agent_id or agent name
    rapidjson::Document doc;
    doc.Parse(view->args.c_str());
    if (doc.HasParseError() || !doc.IsObject())
      continue;

    std::string subagentId;
    std::string subagentTitle;
    std::string subagentTask;

    if (doc.HasMember("title") && doc["title"].IsString()) {
      subagentTitle = doc["title"].GetString();
    }
    if (doc.HasMember("task") && doc["task"].IsString()) {
      subagentTask = doc["task"].GetString();
    }
    if (doc.HasMember("name") && doc["name"].IsString()) {
      if (subagentTitle.empty())
        subagentTitle = doc["name"].GetString();
    }

    // Also check the tool result for agentId
    auto it_result = toolResults.find(toolCallId);
    if (it_result != toolResults.end() && !it_result->second.second.empty()) {
      rapidjson::Document resDoc;
      resDoc.Parse(it_result->second.second.c_str());
      if (!resDoc.HasParseError() && resDoc.IsObject() &&
          resDoc.HasMember("agentId") && resDoc["agentId"].IsString()) {
        subagentId = resDoc["agentId"].GetString();
      }
    }

    // Store title and subagent ID in view
    if (!subagentTitle.empty()) {
      view->subagent_title = subagentTitle;
    }
    if (!subagentId.empty()) {
      view->subagent_id = subagentId;
    }

    // If we have a subagent ID, try to get its history and synthesize the log
    if (!subagentId.empty()) {
      // Register the mapping from subagent to parent tool call
      subagent_to_parent_tool_[subagentId] = toolCallId;

      // Try to get the subagent's history from the harness
      auto &harness = firmius::core::Harness::instance();
      auto subHistoryPtr = harness.getAgentHistoryPtr(subagentId);
      const shared::AgentHistory* subHistory = subHistoryPtr.get();

      // If not available via harness, try loading from disk
      if (!subHistory && !threadId.empty()) {
        auto fallback_hist =
            firmius::core::ThreadManager(
                std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") +
                "/.firmius/threads")
                .loadAgentHistory(threadId, subagentId);
        if (!fallback_hist.turns.empty()) {
          subHistory = new shared::AgentHistory{std::move(fallback_hist)};
        }
      }

      if (subHistory && !subHistory->turns.empty()) {
        // Synthesize subagent_tool_log from subagent's history
        std::vector<shared::SubagentToolLogEntry> logEntries;

        // Add task description as first entry if available
        if (!subagentTask.empty()) {
          shared::SubagentToolLogEntry entry;
          entry.summary = "Task: " + subagentTask;
          entry.phase = shared::ToolPhase::Finished;
          entry.toolCallId = "";
          logEntries.push_back(std::move(entry));
        }

        // Extract tool calls from subagent's history
        for (const auto &turn : subHistory->turns) {
          for (const auto &msg : turn.messages) {
            if (msg.role == shared::Role::Assistant) {
              for (const auto &content : msg.content) {
                if (auto *tc = std::get_if<shared::ToolCallContent>(&content)) {
                  shared::SubagentToolLogEntry entry;
                  entry.name = tc->name;
                  entry.args = tc->args;
                  entry.toolCallId = tc->id;
                  entry.phase = shared::ToolPhase::Finished; // Historical data = completed
                  entry.summary = shared::SummarizeToolCall(tc->name, tc->args, shared::ToolPhase::Finished);
                  logEntries.push_back(std::move(entry));
                } else if (auto *th = std::get_if<shared::ThinkingContent>(&content)) {
                  if (!th->thinking.empty()) {
                    shared::SubagentToolLogEntry entry;
                    entry.summary = "Thought";
                    entry.phase = shared::ToolPhase::Finished;
                    entry.toolCallId = "";
                    logEntries.push_back(std::move(entry));
                  }
                }
              }
            }
          }
        }

        // Add "Done" entry only if the agent actually completed
        if (view->success && view->phase == ToolPhase::Finished) {
          shared::SubagentToolLogEntry doneEntry;
          doneEntry.summary = "Done";
          doneEntry.phase = shared::ToolPhase::Finished;
          doneEntry.toolCallId = "";
          logEntries.push_back(std::move(doneEntry));
        }

        // Limit log size but ensure we keep recent entries
        while (logEntries.size() > 8) {
          logEntries.erase(logEntries.begin());
        }

        view->subagent_tool_log = std::move(logEntries);
        view->subagent_running = false;
      } else {
        // No subagent history available - create minimal log
        // Only add "Done" if the tool call indicates completion
        std::vector<shared::SubagentToolLogEntry> logEntries;
        
        if (!subagentTask.empty()) {
          shared::SubagentToolLogEntry entry;
          entry.summary = "Task: " + subagentTask;
          entry.phase = shared::ToolPhase::Finished;
          entry.toolCallId = "";
          logEntries.push_back(std::move(entry));
        }
        
        // Add "Done" entry only if the tool call indicates completion
        if (view->success && view->phase == ToolPhase::Finished) {
          shared::SubagentToolLogEntry doneEntry;
          doneEntry.summary = "Done";
          doneEntry.phase = shared::ToolPhase::Finished;
          doneEntry.toolCallId = "";
          logEntries.push_back(std::move(doneEntry));
        }
        
        view->subagent_tool_log = std::move(logEntries);
        view->subagent_running = false;
      }
    }
  }
}

const std::vector<std::pair<std::string, std::string>> &
StreamStateManager::getQueuedMessages() const {
  return queued_messages_;
}

int StreamStateManager::getToolCallClusterId(
    const std::string &toolCallId) const {
  auto it = tool_call_cluster_ids_.find(toolCallId);
  if (it == tool_call_cluster_ids_.end()) {
    return -1;
  }
  return it->second;
}

void StreamStateManager::assignToolCallClusterId(
    const std::string &agentId, const std::string &toolCallId) {
  if (tool_call_cluster_ids_.count(toolCallId) > 0) {
    return;
  }
  auto &cluster_state = live_quick_clusters_[agentId];
  if (cluster_state.prose_since_last_tool) {
    cluster_state.current_cluster++;
    cluster_state.prose_since_last_tool = false;
  }
  tool_call_cluster_ids_[toolCallId] = cluster_state.current_cluster;
}

void StreamStateManager::appendLiveTimelineDelta(const std::string &agentId,
                                                 TimelineEntry::Kind kind,
                                                 const std::string &delta) {
  if (delta.empty()) {
    return;
  }

  auto &stream = streams_[agentId];
  if (stream.has_active_live_entry &&
      stream.active_live_entry_kind == kind &&
      !stream.active_live_entry_id.empty()) {
    if (auto *entry = findTimelineEntry(stream.active_live_entry_id)) {
      entry->message += delta;
      return;
    }
    stream.has_active_live_entry = false;
    stream.active_live_entry_id.clear();
  }

  TimelineEntry entry;
  entry.kind = kind;
  entry.agentId = agentId;
  entry.message = delta;
  entry.id =
      "live:" + agentId + ":" + std::to_string(++next_live_entry_sequence_);
  timeline_.push_back(entry);
  stream.active_live_entry_id = entry.id;
  stream.active_live_entry_kind = kind;
  stream.has_active_live_entry = true;
}

void StreamStateManager::clearActiveLiveEntry(const std::string &agentId) {
  auto it = streams_.find(agentId);
  if (it == streams_.end()) {
    return;
  }
  it->second.has_active_live_entry = false;
  it->second.active_live_entry_id.clear();
}

TimelineEntry *StreamStateManager::findTimelineEntry(
    const std::string &entryId) {
  for (auto &entry : timeline_) {
    if (entry.id == entryId) {
      return &entry;
    }
  }
  return nullptr;
}

void StreamStateManager::applyToolResult(
    const std::shared_ptr<ToolCallView> &view, bool success,
    const std::string &result) {
  if (!view) {
    return;
  }

  view->success = success;
  view->result = result;
  if (!success) {
    view->phase = ToolPhase::Error;
    return;
  }

  ParsedProcessResult parsed = parseProcessResult(result);
  if (!parsed.process_id.empty()) {
    view->process_id = parsed.process_id;
    if (process_to_toolcall_.count(parsed.process_id) > 0) {
      process_to_agent_[parsed.process_id] = view->agentId;
      process_finished_state_[parsed.process_id] = false;
    }
  }
  if (parsed.exit_known) {
    view->process_exit_known = true;
    view->process_exit_code = parsed.exit_code;
  }
  view->process_duration_ms = parsed.duration_ms;

  if (view->name == "process_execute" && parsed.finish_reason == "Timeout" &&
      !parsed.process_id.empty()) {
    view->phase = ToolPhase::BackgroundRunning;
    view->process_is_background = true;
    if (process_to_toolcall_.count(parsed.process_id) > 0) {
      process_background_state_[parsed.process_id] = true;
    }
    return;
  }

  if (view->name == "process_spawn" && !parsed.process_id.empty()) {
    view->process_is_background = true;
    if (process_to_toolcall_.count(parsed.process_id) > 0) {
      process_background_state_[parsed.process_id] = true;
    }
  } else if (!parsed.process_id.empty() &&
             process_to_toolcall_.count(parsed.process_id) > 0) {
    process_background_state_[parsed.process_id] = false;
  }

  view->phase = ToolPhase::Finished;
}

} // namespace firmius::tui
