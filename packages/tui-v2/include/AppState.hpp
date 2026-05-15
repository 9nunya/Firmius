#pragma once

#include "Context.hpp"
#include "Events.hpp"
#include "daemon/Protocol.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui2 {

/// A single rendered line in the transcript display.
struct TranscriptLine {
  enum class Kind { UserMessage, AssistantText, Thinking, ToolCall, ToolResult, Notice, System };

  Kind kind = Kind::System;
  std::string text;           ///< Pre-formatted display text (no ANSI).
  std::string agentId;        ///< Which agent produced this line.
  std::string toolCallId;     ///< For tool-related lines.
  std::string toolName;       ///< For tool-related lines.
  bool success = true;        ///< For tool results.
};

/// Active tool call being tracked.
struct ActiveToolCall {
  std::string toolCallId;
  std::string toolName;
  std::string agentId;
  std::string status;         ///< "running", "completed", "failed"
};

/// Connection states.
enum class ConnectionStatus { Disconnected, Connecting, Connected };

/// Agent activity context for keybind decisions.
enum class ActivityContext { Idle, Streaming, PermissionPending };

/// Permission request awaiting user resolution.
struct PendingPermission {
  std::string requestId;
  std::string title;
  std::string message;
  std::string toolName;
  bool allowAlways = true;
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

  void setAgentStatus(firmius::shared::AgentStatus status);
  firmius::shared::AgentStatus agentStatus() const;

  void setModelLabel(const std::string &label);
  std::string modelLabel() const;

  // ── Transcript ──
  void appendTranscriptLine(TranscriptLine line);
  void setTranscriptLines(std::vector<TranscriptLine> lines);
  std::vector<TranscriptLine> transcriptLines() const;
  size_t transcriptLineCount() const;
  size_t lastRenderedLineIndex() const;
  void setLastRenderedLineIndex(size_t index);

  // ── Streaming ──
  void appendStreamingDelta(const std::string &delta);
  void finalizeStreamingLine();
  std::string currentStreamingText() const;
  bool isStreaming() const;

  // ── Tool Calls ──
  void addActiveToolCall(ActiveToolCall call);
  void completeToolCall(const std::string &toolCallId, bool success);
  std::vector<ActiveToolCall> activeToolCalls() const;

  // ── Queued Messages ──
  void setQueuedMessageCount(int count);
  int queuedMessageCount() const;

  // ── Permissions ──
  void setPendingPermission(PendingPermission perm);
  void clearPendingPermission();
  std::optional<PendingPermission> pendingPermission() const;

  // ── Input ──
  void setInputBuffer(const std::string &text);
  std::string inputBuffer() const;
  void appendToInput(char ch);
  void backspaceInput();
  void clearInput();

  // ── Activity Context ──
  ActivityContext activityContext() const;

  // ── Dirty tracking ──
  bool isDirty() const;
  void clearDirty();
  void markDirtyPublic() { markDirty(); }

private:
  void markDirty();

  mutable std::mutex mutex_;
  bool dirty_ = true;

  ConnectionStatus connectionStatus_ = ConnectionStatus::Disconnected;
  std::string threadId_;
  std::string threadTitle_;
  std::string agentId_;
  std::string agentPurpose_;
  std::string agentContextWindow_;
  firmius::shared::AgentStatus agentStatus_ = firmius::shared::AgentStatus::Idle;
  std::string modelLabel_;

  std::vector<TranscriptLine> transcriptLines_;
  size_t lastRenderedLineIndex_ = 0;
  std::string streamingText_;

  std::vector<ActiveToolCall> activeToolCalls_;
  int queuedMessageCount_ = 0;
  std::optional<PendingPermission> pendingPermission_;
  std::string inputBuffer_;
};

} // namespace firmius::tui2
