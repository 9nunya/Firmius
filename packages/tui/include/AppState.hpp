#pragma once

#include "TranscriptItem.hpp"
#include "ToolCallState.hpp"
#include "Context.hpp"
#include "Events.hpp"
#include "daemon/Protocol.hpp"

#include <deque>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::tui {

class AgentTextItem;
class AgentThinkingItem;
class ToolCallItem;
class UserMessageItem;

/// Connection states.
enum class ConnectionStatus { Disconnected, Connecting, Connected };

/// Agent activity context for keybind decisions.
enum class ActivityContext { Idle, Active, PermissionPending };

/// Permission request awaiting user resolution.
using PendingPermissionSuggestion = firmius::shared::PermissionSuggestionWire;

struct PendingPermission {
  std::string requestId;
  std::string title;
  std::string message;
  std::string toolName;
  bool allowAlways = true;
  // ── v2 fields ──
  std::string category;       ///< process.exec, file.read, …
  std::string command;
  std::string commandPrimary;
  std::string targetPath;
  std::string cwd;
  std::string url;
  std::string host;
  std::string scheme;
  std::string query;
  std::string persona;
  std::string parentPersona;
  std::string toolCallId;
  int severity = 0;           ///< CommandSeverity int.
  bool isDirectory = false;
  std::vector<std::string> subcommands;
  std::vector<PendingPermissionSuggestion> suggestions;
};

struct ContextUsage {
  uint32_t windowTokens = 0;
  uint32_t usedTokens = 0;
  uint32_t sentTokens = 0;
};

/// Snapshot of the focused agent's working-memory v2 state.
///
/// Mirrors the fields the status bar and notice-renderer want from
/// shared::MemoryMetrics. Stored separately on AppState so the status
/// bar can render synchronously without locking the agent.
struct MemoryStatus {
  uint32_t rawHistoryTokens = 0;
  uint32_t workingSetTokens = 0;
  uint32_t pinnedTurnCount = 0;
  uint32_t evictedTurnCount = 0;
  uint32_t recalledTurnCount = 0;
  uint32_t deflatedPartCount = 0;
  uint32_t tokensSavedByDeflation = 0;
  uint32_t tokensSavedByEviction = 0;
  uint32_t tokensSpentOnSummaries = 0;
  uint32_t tokensSpentOnEmbeddings = 0;
  uint32_t hotPathLatencyMicros = 0;
  bool aboveBufferThreshold = false;
  bool aboveTargetThreshold = false;
  bool aboveEmergencyThreshold = false;
  bool valid = false; ///< False until the first metrics arrive.
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

  void setPermissionMode(firmius::shared::ThreadPermissionMode mode);
  firmius::shared::ThreadPermissionMode permissionMode() const;
  /// Set the active mode by id (e.g. "ask", "yolo", or user-defined).
  void setActiveModeId(std::string id);
  std::string activeModeId() const;
  /// Set the list of all known modes (id, name, builtIn).
  using ModeSummary = firmius::daemon::PermissionModeWire;
  void setModes(std::vector<ModeSummary> modes);
  std::vector<ModeSummary> modes() const;
  /// Resolve the active mode's display name. Returns "ask" if no
  /// match. Cheap snapshot — for status-bar rendering.
  std::string activeModeName() const;

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

  void setMemoryStatus(MemoryStatus status);
  MemoryStatus memoryStatus() const;

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
  /// Snapshot of the entire queue (front first). Used by the prompt
  /// overlay to draw the [i/N] badge + next-up hint.
  std::vector<PendingPermission> pendingPermissions() const;
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

  // ── Input ──────────────────────────────────────────────────────────────
  //
  // The input buffer is a single std::string with embedded '\n' characters
  // for multiline input. `cursor_` is a byte offset into that string and
  // marks the insertion point. Helpers below convert that offset to/from
  // (line, column) coordinates so renderers and key handlers don't have to
  // re-scan the buffer themselves.
  //
  // Selection-related queries (cursorLineIndex / cursorColumnOnLine) are
  // intentionally byte-based, not visible-width based — that's a separate
  // concern handled at render time. Inputs are typically short enough that
  // recomputing on each keystroke is fine.
  void setInputBuffer(const std::string &text);
  std::string inputBuffer() const;
  /// Append a single byte at the cursor and advance the cursor.
  void appendToInput(char ch);
  /// Insert arbitrary bytes (including newlines) at the cursor and advance.
  void insertAtCursor(const std::string &text);
  /// Delete one byte before the cursor, like a typical Backspace.
  void backspaceInput();
  /// Delete the previous word (alpha+digit run plus any trailing whitespace).
  /// Returns the number of bytes removed. No-op when cursor is at start.
  size_t deleteWordBeforeCursor();
  void clearInput();

  // ── Cursor ──
  /// Cursor as a byte offset into the input buffer.
  size_t cursorOffset() const;
  void setCursorOffset(size_t offset);
  void moveCursorLeft();
  void moveCursorRight();
  /// Move cursor up/down one logical line. Tries to preserve column.
  void moveCursorUp();
  void moveCursorDown();
  /// Move cursor to the previous/next word boundary.
  void moveCursorWordLeft();
  void moveCursorWordRight();
  void moveCursorLineStart();
  void moveCursorLineEnd();
  /// Which logical line of the buffer the cursor is on (0-indexed).
  int cursorLineIndex() const;
  /// Column within that logical line (byte-indexed, 0-based).
  int cursorColumnOnLine() const;
  /// Number of logical lines in the buffer (>= 1).
  int inputLineCount() const;
  /// Get a specific logical line of the buffer.
  std::string inputLineAt(int index) const;

  // ── Pasted blocks ─────────────────────────────────────────────────────
  //
  // When the user pastes a long text block or an image, we don't want to
  // dump the raw bytes into the visible input buffer — the user can't
  // see anything else, and base64 image data would be unusable. Instead
  // we insert a SHORT placeholder string at the cursor (e.g.
  // "[Pasted: 14 lines]") and store the real content here, keyed by a
  // small auto-incrementing id encoded into the placeholder.
  //
  // On submit, the App walks the placeholders in the buffer in
  // appearance order, replaces text-block placeholders with their real
  // content, and drains image blocks into a separate ImageContent[]
  // vector. Empty placeholders left by backspace are simply dropped.
  enum class PastedBlockKind { Text, Image };
  struct PastedBlock {
    int id = 0;
    PastedBlockKind kind = PastedBlockKind::Text;
    std::string content;     ///< For Text: raw pasted text. For Image: base64 (no data:URI prefix).
    std::string mediaType;   ///< For Image: MIME type. Unused for Text.
    int lineCount = 0;       ///< For Text: number of lines in `content`.
  };

  /// Insert a multi-line text paste at the cursor. Returns the placeholder
  /// string that was inserted (e.g. "[Pasted: 14 lines]") so the caller
  /// can show it elsewhere if needed.
  std::string insertPastedText(std::string content);
  /// Insert an image paste at the cursor.
  std::string insertPastedImage(std::string base64, std::string mediaType);
  /// All current blocks (in registration order, not buffer order).
  std::vector<PastedBlock> pastedBlocks() const;
  /// Drain all blocks (used on submit). Caller is responsible for replacing
  /// the placeholders in the outgoing message text.
  std::vector<PastedBlock> takePastedBlocks();
  /// If the cursor sits immediately after a placeholder, remove the whole
  /// placeholder + its block. Returns true if a block was removed.
  bool maybeBackspacePastedBlock();

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

  // ── Mouse-drag selection on the transcript ────────────────────────────
  //
  // Selection coordinates are (absolute scrollback line index, byte column
  // offset on that line). Using ABSOLUTE line indices means scrolling
  // doesn't move the selection — it stays anchored to the actual content.
  //
  // The renderer reads this and inverts the cells inside the range.
  // CopySelected() walks the scrollback inside the range and produces a
  // plain-text string ready for the clipboard.
  struct SelectionPoint {
    int line = -1;   ///< Absolute index into scrollback_, or -1 = no selection.
    int col = 0;     ///< Byte column on that line.
    bool operator==(const SelectionPoint &) const = default;
  };
  void beginSelection(int absLine, int col);
  void updateSelection(int absLine, int col);
  void endSelection();
  void clearSelection();
  bool hasSelection() const;
  bool isSelecting() const;  ///< True while the mouse button is still held.
  /// Returns the selection range with start/end ordered (start <= end in
  /// reading order). Both points are inclusive at the start, exclusive at
  /// the end (like a half-open range).
  std::pair<SelectionPoint, SelectionPoint> selectionRange() const;
  /// Extract the visible selected text from the scrollback, ANSI-stripped.
  std::string copySelectedText() const;

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
  firmius::shared::ThreadPermissionMode permissionMode_ =
      firmius::shared::ThreadPermissionMode::Request;
  std::string activeModeId_ = "ask";
  std::vector<ModeSummary> modes_;
  std::string threadTitle_;
  std::string agentId_;         // Primary (root) agent
  std::string focusedAgentId_;  // Currently focused agent for transcript view
  std::string primaryAgentId_;  // Root agent ID (set once)
  std::string agentPurpose_;
  std::string agentContextWindow_;
  ContextUsage agentContextUsage_;
  MemoryStatus memoryStatus_;
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
  size_t inputCursor_ = 0;
  /// Preferred column when moving up/down through lines. Mirrors the way
  /// most editors remember the column on visual line moves.
  int inputDesiredColumn_ = -1;
  std::vector<PastedBlock> pastedBlocks_;
  int nextPastedBlockId_ = 1;

  // ── Scrollback ──
  std::vector<std::string> scrollback_;
  int scrollOffset_ = 0;       // 0 = at bottom (latest content)
  bool userScrolledUp_ = false; // true when user manually scrolled away from bottom
  int pinnedTopLine_ = -1;    // absolute scrollback index when user scrolled up (-1 = not pinned)
  bool autoScroll_ = true;
  static constexpr int kMaxScrollbackLines = 10000;

  // Selection state. selectionAnchor_/selectionCursor_ both -1 = no
  // selection. While the mouse is held, selectionActive_ stays true so
  // mouse-move events are interpreted as drag-extension.
  SelectionPoint selectionAnchor_{};
  SelectionPoint selectionCursor_{};
  bool selectionActive_ = false;

  // ── Structured state ──
  std::unordered_map<std::string, ToolCallState> toolCalls_;
  std::unordered_map<std::string, std::string> processToTool_;  // processId → toolCallId
  std::unordered_map<std::string, AgentState> agents_;
  EmbeddingDownloadState embeddingDownload_;
};

} // namespace firmius::tui
