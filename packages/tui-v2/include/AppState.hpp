#pragma once

#include "TranscriptItem.hpp"
#include "ToolCallState.hpp"
#include "Context.hpp"
#include "Events.hpp"
#include "daemon/Protocol.hpp"

#include <deque>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::tui2 {

class AgentTextItem;
class AgentThinkingItem;
class ToolCallItem;
class UserMessageItem;

/// Connection states.
enum class ConnectionStatus { Disconnected, Connecting, Connected };

/// Agent activity context for keybind decisions.
enum class ActivityContext { Idle, Active, PermissionPending };

/// Permission request awaiting user resolution.
struct PendingPermission {
  std::string requestId;
  std::string title;
  std::string message;
  std::string toolName;
  bool allowAlways = true;
};

struct ContextUsage {
  uint32_t windowTokens = 0;
  uint32_t usedTokens = 0;
  uint32_t sentTokens = 0;
};

/// Embedding model download progress.
struct EmbeddingDownloadState {
  bool downloading = false;
  std::string modelId;
  uint64_t bytesDownloaded = 0;
  uint64_t totalBytes = 0;
  std::string status;
};

struct AgentActivityEntry {
  std::string key;
  std::string text;
  std::chrono::steady_clock::time_point expiresAt{};
};

/// Track where an item is rendered in the terminal.
struct ItemSpan {
  size_t itemIndex;
  int terminalRow;
  int rowCount;
};

struct QueuedUserMessage {
  std::string messageId;
  std::string text;
  std::string agentId;
};

/// Centralized state store — thread-safe, dirty-tracked.
class AppState {
public:
  AppState() = default;

  // ── Connection ──
  void setConnectionStatus(ConnectionStatus status);
  ConnectionStatus connectionStatus() const;

  // ── Thread / Agent ──
  void setThreadId(const std::string &id);
  std::string threadId() const;

  void setThreadTitle(const std::string &title);
  std::string threadTitle() const;

  void setAgentId(const std::string &id);
  std::string agentId() const;

  void setAgentPurpose(const std::string &purpose);
  std::string agentPurpose() const;

  void setAgentContextWindow(const std::string &ctx);
  std::string agentContextWindow() const;
  void setAgentContextUsage(ContextUsage usage);
  ContextUsage agentContextUsage() const;

  void setAgentStatus(firmius::shared::AgentStatus status);
  firmius::shared::AgentStatus agentStatus() const;

  void setModelLabel(const std::string &label);
  std::string modelLabel() const;
  void setLiveMessage(const std::string &message);
  std::string liveMessage() const;
  void setDaemonReady(bool ready);
  bool daemonReady() const;
  void resetWelcomeState();
  void prepareForThreadLoad();
  void setHookState(const firmius::daemon::HookStateSnapshot &hookState);
  firmius::daemon::HookStateSnapshot hookState() const;

  // ── Items (transcript) ──
  void addItem(std::unique_ptr<TranscriptItem> item);
  void insertItem(size_t index, std::unique_ptr<TranscriptItem> item);
  void clearTranscriptItems();
  void clearItems();
  const std::vector<std::unique_ptr<TranscriptItem>>& items() const;
  size_t itemCount() const;

  // ── Tool call management ──
  ToolCallItem* findToolCallById(const std::string& toolCallId);
  UserMessageItem* findUserMessageById(const std::string& messageId);
  /// Find the last ToolCallItem in the transcript (most recent).
  ToolCallItem* findLastFocusedToolCall();

  // ── Structured tool call state ──
  ToolCallState& getOrCreateToolCall(const std::string& toolCallId);
  ToolCallState* findToolCallState(const std::string& toolCallId);
  const std::unordered_map<std::string, ToolCallState>& toolCalls() const;
  std::unordered_map<std::string, ToolCallState>& toolCallsMut();

  // ── Process → Tool mapping ──
  void mapProcessToTool(const std::string& processId, const std::string& toolCallId);
  std::string findToolCallByProcessId(const std::string& processId) const;

  // ── Agent state ──
  AgentState& getOrCreateAgent(const std::string& agentId);
  AgentState* findAgentState(const std::string& agentId);
  const AgentState* findAgentState(const std::string& agentId) const;
  std::unordered_map<std::string, AgentState>& agentsMut();
  void renameAgent(const std::string& oldId, const std::string& newId);
  void updateAgentModel(const std::string& agentId,
                        const std::string& providerId,
                        const std::string& modelId,
                        uint32_t contextWindowTokens = 0);
  void clearAgentContextUsage(const std::string& agentId);

  // ── Agent focus ──
  void focusAgent(const std::string& agentId);
  std::string focusedAgentId() const;
  void setPrimaryAgentId(const std::string& id);
  std::string primaryAgentId() const;
  std::vector<AgentState> agentList() const;
  std::vector<std::string> siblingsOf(const std::string& agentId) const;
  std::string parentIdOf(const std::string& agentId) const;
  bool hasMultipleAgents() const;

  /// One-line summary of tool calls for an agent (e.g. "Edit, Bash, Read").
  std::string agentToolSummary(const std::string& agentId) const;

  /// Activity log for an agent (last N human-readable lines).
  void appendAgentActivity(const std::string& agentId, const std::string& line);
  void upsertAgentActivity(const std::string& agentId,
                           const std::string& key,
                           const std::string& line,
                           std::chrono::milliseconds ttl = std::chrono::milliseconds{0});
  std::vector<std::string> agentActivityLog(const std::string& agentId, int maxLines = 3) const;

  // ── Streaming management ──
  AgentTextItem* activeTextItem() const;
  void setActiveTextItem(AgentTextItem* item);

  AgentThinkingItem* activeThinkingItem() const;
  void setActiveThinkingItem(AgentThinkingItem* item);

  // Per-agent streaming pointers
  AgentTextItem* agentTextItem(const std::string& agentId) const;
  void setAgentTextItem(const std::string& agentId, AgentTextItem* item);
  AgentThinkingItem* agentThinkingItem(const std::string& agentId) const;
  void setAgentThinkingItem(const std::string& agentId, AgentThinkingItem* item);

  // ── Item span tracking for in-place re-render ──
  const std::vector<ItemSpan>& itemSpans() const;
  void setItemSpans(std::vector<ItemSpan> spans);
  void clearItemSpans();

  // ── Live ticking ──
  bool hasLiveItems() const;
  void markLiveItemsDirty();

  // ── Queued Messages ──
  void setQueuedMessageCount(int count);
  int queuedMessageCount() const;
  void queueMessageId(const std::string& messageId);
  void dequeueMessageId(const std::string& messageId);
  bool isMessageQueued(const std::string& messageId) const;
  void upsertQueuedUserMessage(QueuedUserMessage message);
  void removeQueuedUserMessage(const std::string& messageId);
  std::vector<QueuedUserMessage>
  queuedUserMessagesForAgent(const std::string& agentId) const;

  // ── Permissions (queue to support concurrent requests) ──
  void pushPendingPermission(PendingPermission perm);
  void popPendingPermission(const std::string &requestId);
  void clearPendingPermissions();
  std::optional<PendingPermission> pendingPermission() const;
  bool hasPendingPermissions() const;

  // ── Embedding ──
  void setEmbeddingDownload(EmbeddingDownloadState state);
  EmbeddingDownloadState embeddingDownload() const;

  // ── Todos ──
  void setAgentTodos(const std::string& agentId,
                     const std::vector<firmius::shared::TodoItem>& items);
  std::vector<firmius::shared::TodoItem> agentTodos(const std::string& agentId) const;
  std::vector<firmius::shared::TodoItem> focusedAgentTodos() const;
  void clearTodos();
  void toggleTodoVisibility();
  bool todoVisible() const;

  // ── Input ──
  void setInputBuffer(const std::string &text);
  std::string inputBuffer() const;
  void appendToInput(char ch);
  void backspaceInput();
  void clearInput();

  // ── Activity Context ──
  ActivityContext activityContext() const;

  // ── Scrollback ──
  void appendScrollback(const std::vector<std::string>& lines);
  void removeTrailingScrollback(int count);
  void clearScrollback();
  const std::vector<std::string>& scrollback() const;
  int scrollbackSize() const;

  void scrollUp(int amount);
  void scrollDown(int amount);
  void scrollToTop();
  void scrollToBottom();
  int scrollOffset() const;
  bool isAtBottom() const;
  /// Whether the user has scrolled away from the bottom (disables auto-follow).
  bool userScrolledUp() const { return userScrolledUp_; }
  void setAutoScroll(bool enabled);

  // ── Dirty tracking ──
  bool isDirty() const;
  void clearDirty();
  bool consumeFullResyncRequested();
  void markDirtyPublic() { markDirty(); }

private:
  void markDirty();

  mutable std::mutex mutex_;
  bool dirty_ = true;
  bool fullResyncRequested_ = false;

  ConnectionStatus connectionStatus_ = ConnectionStatus::Disconnected;
  std::string threadId_;
  std::string threadTitle_;
  std::string agentId_;         // Primary (root) agent
  std::string focusedAgentId_;  // Currently focused agent for transcript view
  std::string primaryAgentId_;  // Root agent ID (set once)
  std::string agentPurpose_;
  std::string agentContextWindow_;
  ContextUsage agentContextUsage_;
  firmius::shared::AgentStatus agentStatus_ = firmius::shared::AgentStatus::Idle;
  std::string modelLabel_;
  std::string liveMessage_;
  bool daemonReady_ = false;
  firmius::daemon::HookStateSnapshot hookState_;

  // ── Item-based transcript ──
  std::vector<std::unique_ptr<TranscriptItem>> items_;
  std::vector<ItemSpan> itemSpans_;

  // ── Streaming pointers (not owned) ──
  // Global pointers for the focused agent (kept for backward compat)
  AgentTextItem* activeTextItem_ = nullptr;
  AgentThinkingItem* activeThinkingItem_ = nullptr;
  // Per-agent streaming pointers
  std::unordered_map<std::string, AgentTextItem*> agentTextItems_;
  std::unordered_map<std::string, AgentThinkingItem*> agentThinkingItems_;
  // Per-agent activity log (human-readable lines)
  std::unordered_map<std::string, std::vector<AgentActivityEntry>> agentActivityLogs_;

  int queuedMessageCount_ = 0;
  std::unordered_set<std::string> queuedMessageIds_;
  std::vector<QueuedUserMessage> queuedUserMessages_;
  std::deque<PendingPermission> pendingPermissions_;
  std::unordered_map<std::string, std::vector<firmius::shared::TodoItem>> agentTodos_;
  bool todoVisible_ = true;
  std::string inputBuffer_;

  // ── Scrollback ──
  std::vector<std::string> scrollback_;
  int scrollOffset_ = 0;       // 0 = at bottom (latest content)
  bool userScrolledUp_ = false; // true when user manually scrolled away from bottom
  int pinnedTopLine_ = -1;    // absolute scrollback index when user scrolled up (-1 = not pinned)
  bool autoScroll_ = true;
  static constexpr int kMaxScrollbackLines = 10000;

  // ── Structured state ──
  std::unordered_map<std::string, ToolCallState> toolCalls_;
  std::unordered_map<std::string, std::string> processToTool_;  // processId → toolCallId
  std::unordered_map<std::string, AgentState> agents_;
  EmbeddingDownloadState embeddingDownload_;
};

} // namespace firmius::tui2
