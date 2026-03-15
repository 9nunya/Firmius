#include "StreamStateManager.hpp"
#include "components/ToolBlock.hpp"
#include "utils/ToolSummaries.hpp"
#include "harness/Harness.hpp"
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <rapidjson/document.h>

namespace firmius::tui {

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
  pushTokenUsage(e.agentId, e.aggregateMetrics);

  // Process tool results from the turn to determine success/failure
  std::unordered_map<std::string, bool> toolSuccessMap;
  for (const auto &msg : e.turn.messages) {
    if (msg.role == shared::Role::ToolResult) {
      for (const auto &content : msg.content) {
        if (auto *trc = std::get_if<shared::ToolResultContent>(&content)) {
          toolSuccessMap[trc->toolCallId] = trc->success;
        }
      }
    }
  }

  // Mark all pending tool calls for this agent as finished with proper success state
  for (auto &[toolId, view] : tool_calls_) {
    if (view && view->agentId == e.agentId &&
        view->name != "summon_subagent" &&
        view->phase != ToolPhase::Finished) {
      auto it = toolSuccessMap.find(toolId);
      if (it != toolSuccessMap.end()) {
        view->success = it->second;
        view->phase = it->second ? ToolPhase::Finished : ToolPhase::Error;
      } else {
        // No result found, assume success
        view->phase = ToolPhase::Finished;
        view->success = true;
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
        it->second->name != "summon_subagent") {
      it = tool_calls_.erase(it);
    } else {
      ++it;
    }
  }
  timeline_.erase(std::remove_if(timeline_.begin(), timeline_.end(),
                                 [&](const TimelineEntry &entry) {
                                   if (entry.kind !=
                                       TimelineEntry::Kind::ToolCall)
                                     return false;
                                   return entry.agentId == e.agentId;
                                 }),
                  timeline_.end());
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

  auto &view = tool_calls_[e.toolCallId];
  if (!view) {
    view = std::make_shared<ToolCallView>();
    view->toolCallId = e.toolCallId;
    view->agentId = e.agentId;
    timeline_.push_back(
        {TimelineEntry::Kind::ToolCall, e.toolCallId, "", e.agentId});
  }
  view->phase = ToolPhase::Preparing;
  view->name += e.nameDelta;
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
  auto &view = tool_calls_[e.toolCallId];
  if (!view) {
    view = std::make_shared<ToolCallView>();
    view->toolCallId = e.toolCallId;
    timeline_.push_back(
        {TimelineEntry::Kind::ToolCall, e.toolCallId, "", e.agentId});
  }
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
}

void StreamStateManager::rebuildToolCallsFromHistory(
    const std::string &agentId, const shared::AgentHistory *history,
    const std::string &threadId, bool populate_subagent_log) {
  if (!history)
    return;

  // First pass: collect all tool results to know success/failure
  std::unordered_map<std::string, std::pair<bool, std::string>> toolResults;
  for (const auto &turn : history->turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == shared::Role::ToolResult) {
        for (const auto &content : msg.content) {
          if (auto *tr = std::get_if<shared::ToolResultContent>(&content)) {
            toolResults[tr->toolCallId] = {tr->success, tr->result};
          }
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
                view->success = it->second.first;
                view->result = it->second.second;
                view->phase = it->second.first ? ToolPhase::Finished
                                               : ToolPhase::Error;
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

} // namespace firmius::tui
