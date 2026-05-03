#ifndef FIRMIUS_STREAM_STATE_MANAGER_HPP
#define FIRMIUS_STREAM_STATE_MANAGER_HPP

#include "Events.hpp"
#include "tools/ProcessState.hpp"
#include "tools/SubagentState.hpp"
#include "utils/ToolView.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace firmius::tui {

using firmius::shared::ToolCallView;

struct TimelineEntry {
  enum class Kind { Thinking, Text, ToolCall, Error };
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
  std::string compaction_completion;
  bool compaction_active = false;
  bool compaction_finished = false;
  bool provider_waiting = false;
  std::chrono::steady_clock::time_point thinking_start{};
  bool is_thinking = false;
  std::string active_live_entry_id;
  TimelineEntry::Kind active_live_entry_kind = TimelineEntry::Kind::Text;
  bool has_active_live_entry = false;
  // Hot-path optimization (H1): direct index into timeline_ for the active
  // live entry, valid only while has_active_live_entry == true. Eliminates
  // per-token O(N) scans through the timeline vector.
  std::size_t active_live_entry_index = 0;
};

struct ProcessCounts {
  int live = 0;
  int background = 0;
};

struct ProcessRuntimeSnapshot {
  std::vector<std::string> owned_process_ids;
  std::vector<std::string> blocking_process_ids;
};

struct QueuedMessageEntry {
  std::string message_id;
  std::string text;
  std::string thread_id;
  std::string agent_id;
  int image_count = 0;
};

struct CompletedRunSummary {
  std::string text;
};

class StreamStateManager {
public:
  void handleAgentThinking(const shared::AgentThinking &e);
  void handleAgentText(const shared::AgentText &e);
  void handleAgentTurnCompleted(const shared::AgentTurnCompleted &e);
  void handleAgentCompacting(const shared::AgentCompacting &e);
  void handleAgentProviderWaiting(const shared::AgentProviderWaiting &e);
  void handleAgentToolCallChunk(const shared::AgentToolCallChunk &e);
  void handleAgentToolCall(const shared::AgentToolCall &e);
  void handleAgentFileEdited(const shared::AgentFileEdited &e);
  void handleAgentCompactionThinking(const shared::AgentCompactionThinking &e);
  void handleAgentCompactionText(const shared::AgentCompactionText &e);
  void handleContextCompacted(const shared::ContextCompacted &e);
  void handleAgentProcessSpawned(const shared::AgentProcessSpawned &e);
  void handleAgentProcessOutput(const shared::AgentProcessOutput &e);
  void handleAgentFinished(const shared::AgentFinished &e);
  void handleAgentInterrupted(const shared::AgentInterrupted &e);
  void handleAgentError(const shared::AgentError &e);
  void handleAgentSpawned(const shared::AgentSpawned &e,
                          const std::string &focused_agent_id);
  void handleAgentRetrying(const shared::AgentRetrying &e);
  void handleAgentRetryFailed(const shared::AgentRetryFailed &e);
  void handleAgentAccountSwitched(const shared::AgentAccountSwitched &e);
  void handleMessageQueued(const shared::MessageQueued &e);
  void handleMessageDequeued(const shared::MessageDequeued &e);
  void handleInternalMessageQueued(const shared::InternalMessageQueued &e);
  void handleInternalMessageDequeued(const shared::InternalMessageDequeued &e);
  void handleThreadChanged();

  // Rebuild tool calls from history when loading a thread
  void rebuildToolCallsFromHistory(const std::string &agentId,
                                   const shared::AgentHistory *history,
                                   const std::string &threadId,
                                   bool populate_subagent_log = true);

  const StreamState *getStream(const std::string &agentId) const;
  const std::vector<TimelineEntry> &getTimeline() const;
  // Monotonic counter used to memoize expensive live-row rendering.
  // Incremented whenever live/timeline/tool/queue state changes.
  uint64_t getLiveRenderEpoch() const;
  const std::unordered_map<std::string, std::shared_ptr<ToolCallView>> &
  getToolCalls() const;
  std::shared_ptr<ToolCallView>
  getToolView(const std::string &toolCallId) const;
  std::string getAgentTitle(const std::string &agentId) const;
  ProcessCounts getProcessCounts(const std::string &agentId) const;
  ProcessCounts getProcessCounts(
      const std::string &agentId, const ProcessRuntimeSnapshot *runtime_snapshot,
      const std::function<bool(const std::string &)> &is_process_running) const;
  const NormalizedProcessState *
  getProcessState(const std::string &processId) const;
  const NormalizedProcessState *
  getProcessStateForToolCall(const std::string &toolCallId) const;
  const NormalizedSubagentState *
  getSubagentState(const std::string &parentToolCallId) const;
  const NormalizedSubagentState *
  getSubagentStateForToolCall(const std::string &toolCallId) const;
  const std::string &getRetryStatus() const;
  const std::vector<std::string> &getAccountSwaps() const;
  const std::vector<QueuedMessageEntry> &getQueuedMessages() const;
  const std::vector<QueuedMessageEntry> &getQueuedInternalMessages() const;
  int getToolCallClusterId(const std::string &toolCallId) const;
  const shared::AgentMetrics *getLatestMetrics(const std::string &agentId) const;
  const std::vector<CompletedRunSummary> *
  getCompletedRunSummaries(const std::string &agentId) const;

private:
  struct LiveQuickClusterState {
    int current_cluster = 0;
    bool prose_since_last_tool = false;
  };

  void pushThinkingDuration(const std::string &agentId, float seconds);
  void pushTokenUsage(const std::string &agentId,
                      const shared::AgentMetrics &metrics);
  void clearRetryStatus();
  void reactivateSubagentParentView(const std::string &agentId);
  void applyToolResult(const std::shared_ptr<ToolCallView> &view, bool success,
                       const std::string &result);
  void assignToolCallClusterId(const std::string &agentId,
                               const std::string &toolCallId);
  void markLiveStateChanged();
  void appendLiveTimelineDelta(const std::string &agentId, TimelineEntry::Kind kind,
                               const std::string &delta);
  void clearActiveLiveEntry(const std::string &agentId);
  TimelineEntry *findTimelineEntry(const std::string &entryId);
  bool applyProcessOutputToToolView(const shared::AgentProcessOutput &e);
  void flushBufferedProcessOutputForProcess(const std::string &processId);
  void flushBufferedProcessOutputForToolCall(const std::string &toolCallId);

  std::unordered_map<std::string, StreamState> streams_;
  std::unordered_map<std::string, std::shared_ptr<ToolCallView>> tool_calls_;
  std::vector<TimelineEntry> timeline_;
  uint64_t next_live_entry_sequence_ = 0;

  uint64_t live_render_epoch_ = 1;

  std::unordered_map<std::string, std::string> process_outputs_;
  std::unordered_map<std::string, std::string> subagent_outputs_;
  std::unordered_map<std::string, std::string> process_to_toolcall_;
  std::unordered_map<std::string, std::string> current_process_for_agent_;
  std::unordered_map<std::string, std::string> current_subagent_for_agent_;
  std::unordered_map<std::string, std::string> subagent_to_parent_tool_;
  std::unordered_map<std::string, NormalizedSubagentState> subagent_state_;
  std::unordered_map<std::string, std::string> subagent_tool_to_parent_;
  std::unordered_map<std::string, std::string> agent_titles_;
  std::unordered_map<std::string, std::string> agent_provider_model_;
  std::unordered_map<std::string, std::string> process_to_agent_;
  std::unordered_map<std::string, NormalizedProcessState> process_state_;
  std::unordered_map<std::string, shared::AgentMetrics> latest_metrics_;
  std::unordered_map<std::string, std::vector<CompletedRunSummary>>
      completed_run_summaries_;
  std::unordered_map<std::string, std::string> last_todo_result_by_agent_;
  std::unordered_map<std::string, std::vector<shared::AgentProcessOutput>>
      pending_process_output_;
  std::unordered_map<std::string, LiveQuickClusterState> live_quick_clusters_;
  std::unordered_map<std::string, int> tool_call_cluster_ids_;

  std::string retry_status_;
  std::vector<std::string> account_swaps_;
  std::vector<QueuedMessageEntry> queued_messages_;
  std::vector<QueuedMessageEntry> queued_internal_messages_;
};

} // namespace firmius::tui

#endif
