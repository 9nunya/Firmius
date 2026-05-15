#include "AppState.hpp"

#include <cctype>

namespace firmius::tui2 {

void AppState::markDirty() { dirty_ = true; }

// ── Connection ──

void AppState::setConnectionStatus(ConnectionStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  connectionStatus_ = status;
  markDirty();
}

ConnectionStatus AppState::connectionStatus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connectionStatus_;
}

// ── Thread / Agent ──

void AppState::setThreadId(const std::string &id) {
  std::lock_guard<std::mutex> lock(mutex_);
  threadId_ = id;
  markDirty();
}

std::string AppState::threadId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return threadId_;
}

void AppState::setThreadTitle(const std::string &title) {
  std::lock_guard<std::mutex> lock(mutex_);
  threadTitle_ = title;
  markDirty();
}

std::string AppState::threadTitle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return threadTitle_;
}

void AppState::setAgentId(const std::string &id) {
  std::lock_guard<std::mutex> lock(mutex_);
  agentId_ = id;
  markDirty();
}

std::string AppState::agentId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return agentId_;
}

void AppState::setAgentPurpose(const std::string &purpose) {
  std::lock_guard<std::mutex> lock(mutex_);
  agentPurpose_ = purpose;
  markDirty();
}

std::string AppState::agentPurpose() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return agentPurpose_;
}

void AppState::setAgentContextWindow(const std::string &ctx) {
  std::lock_guard<std::mutex> lock(mutex_);
  agentContextWindow_ = ctx;
  markDirty();
}

std::string AppState::agentContextWindow() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return agentContextWindow_;
}

void AppState::setAgentStatus(firmius::shared::AgentStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  agentStatus_ = status;
  markDirty();
}

firmius::shared::AgentStatus AppState::agentStatus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return agentStatus_;
}

void AppState::setModelLabel(const std::string &label) {
  std::lock_guard<std::mutex> lock(mutex_);
  modelLabel_ = label;
  markDirty();
}

std::string AppState::modelLabel() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return modelLabel_;
}

// ── Transcript ──

void AppState::appendTranscriptLine(TranscriptLine line) {
  std::lock_guard<std::mutex> lock(mutex_);
  transcriptLines_.push_back(std::move(line));
  markDirty();
}

void AppState::setTranscriptLines(std::vector<TranscriptLine> lines) {
  std::lock_guard<std::mutex> lock(mutex_);
  transcriptLines_ = std::move(lines);
  lastRenderedLineIndex_ = 0;
  markDirty();
}

std::vector<TranscriptLine> AppState::transcriptLines() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return transcriptLines_;
}

size_t AppState::transcriptLineCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return transcriptLines_.size();
}

size_t AppState::lastRenderedLineIndex() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lastRenderedLineIndex_;
}

void AppState::setLastRenderedLineIndex(size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  lastRenderedLineIndex_ = index;
}

// ── Streaming ──

void AppState::flushThinkingBufferLocked() {
  if (streamingThinkingText_.empty()) return;

  auto flushLine = [this](std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
      text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
      text.pop_back();
    }
    if (text.empty()) return;
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::Thinking;
    line.text = std::move(text);
    transcriptLines_.push_back(std::move(line));
  };

  size_t pos = 0;
  while ((pos = streamingThinkingText_.find('\n')) != std::string::npos) {
    flushLine(streamingThinkingText_.substr(0, pos));
    streamingThinkingText_ = streamingThinkingText_.substr(pos + 1);
  }

  constexpr size_t kStreamingWrapColumn = 88;
  while (streamingThinkingText_.size() >= kStreamingWrapColumn) {
    size_t split = streamingThinkingText_.rfind(' ', kStreamingWrapColumn);
    if (split == std::string::npos || split == 0) {
      split = kStreamingWrapColumn;
    }
    flushLine(streamingThinkingText_.substr(0, split));
    streamingThinkingText_ = streamingThinkingText_.substr(split);
  }

  if (!streamingThinkingText_.empty()) {
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::Thinking;
    line.text = std::move(streamingThinkingText_);
    transcriptLines_.push_back(std::move(line));
    streamingThinkingText_.clear();
  }
}

void AppState::appendStreamingDelta(const std::string &delta) {
  std::lock_guard<std::mutex> lock(mutex_);
  flushThinkingBufferLocked();
  streamingText_ += delta;

  auto flushAssistantLine = [this](std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
      text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
      text.pop_back();
    }
    if (text.empty()) return;
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::AssistantText;
    line.text = std::move(text);
    transcriptLines_.push_back(std::move(line));
  };

  size_t pos = 0;
  while ((pos = streamingText_.find('\n')) != std::string::npos) {
    flushAssistantLine(streamingText_.substr(0, pos));
    streamingText_ = streamingText_.substr(pos + 1);
  }

  constexpr size_t kStreamingWrapColumn = 88;
  while (streamingText_.size() >= kStreamingWrapColumn) {
    size_t split = streamingText_.rfind(' ', kStreamingWrapColumn);
    if (split == std::string::npos || split == 0) {
      split = kStreamingWrapColumn;
    }
    flushAssistantLine(streamingText_.substr(0, split));
    streamingText_ = streamingText_.substr(split);
  }

  markDirty();
}

void AppState::finalizeStreamingLine() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!streamingText_.empty()) {
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::AssistantText;
    line.text = std::move(streamingText_);
    transcriptLines_.push_back(std::move(line));
    streamingText_.clear();
  }
  agentStatus_ = firmius::shared::AgentStatus::Idle;
  markDirty();
}

void AppState::appendStreamingThinkingDelta(const std::string &delta) {
  std::lock_guard<std::mutex> lock(mutex_);
  streamingThinkingText_ += delta;
  markDirty();
}

void AppState::finalizeStreamingThinkingLine() {
  std::lock_guard<std::mutex> lock(mutex_);
  flushThinkingBufferLocked();
  markDirty();
}

std::string AppState::currentStreamingText() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return streamingText_;
}

bool AppState::isStreaming() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !streamingText_.empty() ||
         agentStatus_ == firmius::shared::AgentStatus::Streaming;
}

// ── Tool Calls ──

void AppState::addActiveToolCall(ActiveToolCall call) {
  std::lock_guard<std::mutex> lock(mutex_);
  activeToolCalls_.push_back(std::move(call));
  markDirty();
}

void AppState::completeToolCall(const std::string &toolCallId, bool success) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = activeToolCalls_.begin(); it != activeToolCalls_.end(); ++it) {
    if (it->toolCallId == toolCallId) {
      // Record in transcript as completed.
      TranscriptLine line;
      line.kind = TranscriptLine::Kind::ToolResult;
      line.toolCallId = toolCallId;
      line.toolName = it->toolName;
      line.success = success;
      line.text = (success ? "✓ " : "✗ ") + it->toolName;
      transcriptLines_.push_back(std::move(line));
      activeToolCalls_.erase(it);
      markDirty();
      return;
    }
  }
}

std::vector<ActiveToolCall> AppState::activeToolCalls() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return activeToolCalls_;
}

// ── Queued Messages ──

void AppState::setQueuedMessageCount(int count) {
  std::lock_guard<std::mutex> lock(mutex_);
  queuedMessageCount_ = count;
  markDirty();
}

int AppState::queuedMessageCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queuedMessageCount_;
}

// ── Permissions ──

void AppState::setPendingPermission(PendingPermission perm) {
  std::lock_guard<std::mutex> lock(mutex_);
  pendingPermission_ = std::move(perm);
  markDirty();
}

void AppState::clearPendingPermission() {
  std::lock_guard<std::mutex> lock(mutex_);
  pendingPermission_.reset();
  markDirty();
}

std::optional<PendingPermission> AppState::pendingPermission() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pendingPermission_;
}

// ── Input ──

void AppState::setInputBuffer(const std::string &text) {
  std::lock_guard<std::mutex> lock(mutex_);
  inputBuffer_ = text;
  markDirty();
}

std::string AppState::inputBuffer() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return inputBuffer_;
}

void AppState::appendToInput(char ch) {
  std::lock_guard<std::mutex> lock(mutex_);
  inputBuffer_ += ch;
  markDirty();
}

void AppState::backspaceInput() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!inputBuffer_.empty()) {
    inputBuffer_.pop_back();
    markDirty();
  }
}

void AppState::clearInput() {
  std::lock_guard<std::mutex> lock(mutex_);
  inputBuffer_.clear();
  markDirty();
}

// ── Activity Context ──

ActivityContext AppState::activityContext() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pendingPermission_.has_value()) {
    return ActivityContext::PermissionPending;
  }
  if (agentStatus_ == firmius::shared::AgentStatus::Streaming ||
      !streamingText_.empty()) {
    return ActivityContext::Streaming;
  }
  return ActivityContext::Idle;
}

// ── Dirty tracking ──

bool AppState::isDirty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dirty_;
}

void AppState::clearDirty() {
  std::lock_guard<std::mutex> lock(mutex_);
  dirty_ = false;
}

} // namespace firmius::tui2
