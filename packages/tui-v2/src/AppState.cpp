#include "AppState.hpp"
#include "items/ToolCallItem.hpp"
#include "items/StreamingItems.hpp"

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

// ── Items (transcript) ──

void AppState::addItem(std::unique_ptr<TranscriptItem> item) {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.push_back(std::move(item));
  markDirty();
}

void AppState::clearItems() {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.clear();
  toolCalls_.clear();
  processToTool_.clear();
  agents_.clear();
  activeTextItem_ = nullptr;
  activeThinkingItem_ = nullptr;
  scrollback_.clear();
  scrollOffset_ = 0;
  userScrolledUp_ = false;
  pinnedTopLine_ = -1;
  markDirty();
}

const std::vector<std::unique_ptr<TranscriptItem>>& AppState::items() const {
  // NOTE: caller must not hold lock — this returns a reference.
  // The mutex_ is NOT locked here for performance; callers should
  // use this during render which is single-threaded.
  return items_;
}

size_t AppState::itemCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_.size();
}

// ── Tool call management ──

ToolCallItem* AppState::findToolCallById(const std::string& toolCallId) {
  for (auto& item : items_) {
    if (item->type() == "ToolCall") {
      auto* tc = static_cast<ToolCallItem*>(item.get());
      if (tc->toolCallId() == toolCallId) {
        return tc;
      }
    }
  }
  return nullptr;
}

ToolCallItem* AppState::findLastFocusedToolCall() {
  for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
    if ((*it)->type() == "ToolCall") {
      return static_cast<ToolCallItem*>(it->get());
    }
  }
  return nullptr;
}

// ── Structured tool call state ──

ToolCallState& AppState::getOrCreateToolCall(const std::string& toolCallId) {
  return toolCalls_[toolCallId];
}

ToolCallState* AppState::findToolCallState(const std::string& toolCallId) {
  auto it = toolCalls_.find(toolCallId);
  return (it != toolCalls_.end()) ? &it->second : nullptr;
}

const std::unordered_map<std::string, ToolCallState>& AppState::toolCalls() const {
  return toolCalls_;
}

std::unordered_map<std::string, ToolCallState>& AppState::toolCallsMut() {
  return toolCalls_;
}

// ── Process → Tool mapping ──

void AppState::mapProcessToTool(const std::string& processId, const std::string& toolCallId) {
  processToTool_[processId] = toolCallId;
}

std::string AppState::findToolCallByProcessId(const std::string& processId) const {
  auto it = processToTool_.find(processId);
  return (it != processToTool_.end()) ? it->second : std::string();
}

// ── Agent state ──

AgentState& AppState::getOrCreateAgent(const std::string& agentId) {
  return agents_[agentId];
}

AgentState* AppState::findAgentState(const std::string& agentId) {
  auto it = agents_.find(agentId);
  return (it != agents_.end()) ? &it->second : nullptr;
}

const AgentState* AppState::findAgentState(const std::string& agentId) const {
  auto it = agents_.find(agentId);
  return (it != agents_.end()) ? &it->second : nullptr;
}

// ── Agent focus ──

void AppState::focusAgent(const std::string& agentId) {
  std::lock_guard<std::mutex> lock(mutex_);
  focusedAgentId_ = agentId;
  // Update StatusBar fields from the focused agent's state
  auto it = agents_.find(agentId);
  if (it != agents_.end()) {
    agentPurpose_ = it->second.personaName;
    if (!it->second.modelId.empty()) {
      modelLabel_ = it->second.providerId + "/" + it->second.modelId;
    }
    agentStatus_ = it->second.status;
  }
  markDirty();
}

std::string AppState::focusedAgentId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!focusedAgentId_.empty()) return focusedAgentId_;
  return agentId_;  // Fallback to primary agent
}

void AppState::setPrimaryAgentId(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  primaryAgentId_ = id;
}

std::string AppState::primaryAgentId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return primaryAgentId_;
}

std::vector<AgentState*> AppState::agentList() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AgentState*> result;
  result.reserve(agents_.size());
  for (auto& [id, agent] : agents_) {
    result.push_back(const_cast<AgentState*>(&agent));
  }
  return result;
}

std::vector<std::string> AppState::siblingsOf(const std::string& agentId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = agents_.find(agentId);
  if (it == agents_.end()) return {};

  const auto& agent = it->second;

  // Check if this agent has children
  std::vector<std::string> children;
  for (const auto& [id, a] : agents_) {
    if (a.parentId == agentId) children.push_back(id);
  }

  if (!children.empty()) {
    // Cycle among children
    return children;
  }

  // Cycle among siblings (same parentId), including self
  std::vector<std::string> siblings;
  for (const auto& [id, a] : agents_) {
    if (a.parentId == agent.parentId) siblings.push_back(id);
  }
  return siblings;
}

std::string AppState::parentIdOf(const std::string& agentId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = agents_.find(agentId);
  if (it == agents_.end()) return {};
  return it->second.parentId;
}

bool AppState::hasMultipleAgents() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return agents_.size() > 1;
}

// ── Streaming management ──

AgentTextItem* AppState::activeTextItem() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return activeTextItem_;
}

void AppState::setActiveTextItem(AgentTextItem* item) {
  std::lock_guard<std::mutex> lock(mutex_);
  activeTextItem_ = item;
}

AgentThinkingItem* AppState::activeThinkingItem() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return activeThinkingItem_;
}

void AppState::setActiveThinkingItem(AgentThinkingItem* item) {
  std::lock_guard<std::mutex> lock(mutex_);
  activeThinkingItem_ = item;
}

// ── Item span tracking ──

const std::vector<ItemSpan>& AppState::itemSpans() const {
  return itemSpans_;
}

void AppState::setItemSpans(std::vector<ItemSpan> spans) {
  std::lock_guard<std::mutex> lock(mutex_);
  itemSpans_ = std::move(spans);
}

void AppState::clearItemSpans() {
  std::lock_guard<std::mutex> lock(mutex_);
  itemSpans_.clear();
}

// ── Live ticking ──

bool AppState::hasLiveItems() const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& item : items_) {
    if (item->type() == "ToolCall") {
      auto* tc = static_cast<const ToolCallItem*>(item.get());
      if (tc->isLive()) return true;
    }
  }
  return false;
}

void AppState::markLiveItemsDirty() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : items_) {
    if (item->type() == "ToolCall") {
      auto* tc = static_cast<ToolCallItem*>(item.get());
      if (tc->isLive()) {
        tc->markDirty();
      }
    }
  }
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

// ── Permissions (queue) ──

void AppState::pushPendingPermission(PendingPermission perm) {
  std::lock_guard<std::mutex> lock(mutex_);
  pendingPermissions_.push_back(std::move(perm));
  markDirty();
}

void AppState::popPendingPermission(const std::string &requestId) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = pendingPermissions_.begin(); it != pendingPermissions_.end(); ++it) {
    if (it->requestId == requestId) {
      pendingPermissions_.erase(it);
      markDirty();
      return;
    }
  }
}

void AppState::clearPendingPermissions() {
  std::lock_guard<std::mutex> lock(mutex_);
  pendingPermissions_.clear();
  markDirty();
}

std::optional<PendingPermission> AppState::pendingPermission() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pendingPermissions_.empty()) return std::nullopt;
  return pendingPermissions_.front();
}

bool AppState::hasPendingPermissions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !pendingPermissions_.empty();
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
  if (!pendingPermissions_.empty()) {
    return ActivityContext::PermissionPending;
  }
  // Agent is "active" (interruptible) unless it's in an inert state.
  if (agentStatus_ != firmius::shared::AgentStatus::Idle &&
      agentStatus_ != firmius::shared::AgentStatus::Cancelled &&
      agentStatus_ != firmius::shared::AgentStatus::Error) {
    return ActivityContext::Active;
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

// ── Scrollback ──

void AppState::appendScrollback(const std::vector<std::string>& lines) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& line : lines) {
    scrollback_.push_back(line);
  }
  // Trim from front if over limit.
  if (static_cast<int>(scrollback_.size()) > kMaxScrollbackLines) {
    int excess = static_cast<int>(scrollback_.size()) - kMaxScrollbackLines;
    scrollback_.erase(scrollback_.begin(), scrollback_.begin() + excess);
    scrollOffset_ = std::max(0, scrollOffset_ - excess);
    if (pinnedTopLine_ >= 0) {
      pinnedTopLine_ = std::max(0, pinnedTopLine_ - excess);
    }
  }
  // If user scrolled up and viewport is pinned, adjust offset to keep
  // the viewport at the same absolute content position.
  if (userScrolledUp_ && pinnedTopLine_ >= 0) {
    int totalLines = static_cast<int>(scrollback_.size());
    scrollOffset_ = totalLines - pinnedTopLine_;
    if (scrollOffset_ < 0) scrollOffset_ = 0;
  }
  markDirty();
}

void AppState::removeTrailingScrollback(int count) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count <= 0) return;
  int removeCount = std::min(count, static_cast<int>(scrollback_.size()));
  scrollback_.erase(scrollback_.end() - removeCount, scrollback_.end());
  // Adjust offset so viewport stays stable.
  scrollOffset_ = std::max(0, scrollOffset_ - removeCount);
  markDirty();
}

void AppState::clearScrollback() {
  std::lock_guard<std::mutex> lock(mutex_);
  scrollback_.clear();
  scrollOffset_ = 0;
  autoScroll_ = true;
  markDirty();
}

const std::vector<std::string>& AppState::scrollback() const {
  // Note: caller must not hold mutex (this returns a reference).
  // In practice, only called from the render thread which already holds the lock
  // via the main loop. For safety, we don't lock here since the render path
  // is single-threaded.
  return scrollback_;
}

int AppState::scrollbackSize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(scrollback_.size());
}

void AppState::scrollUp(int amount) {
  std::lock_guard<std::mutex> lock(mutex_);
  scrollOffset_ += amount;
  if (scrollOffset_ > static_cast<int>(scrollback_.size())) {
    scrollOffset_ = static_cast<int>(scrollback_.size());
  }
  autoScroll_ = false;
  userScrolledUp_ = true;
  // Pin the viewport so new content doesn't push the user down.
  int totalLines = static_cast<int>(scrollback_.size());
  pinnedTopLine_ = totalLines - scrollOffset_;
  markDirty();
}

void AppState::scrollDown(int amount) {
  std::lock_guard<std::mutex> lock(mutex_);
  scrollOffset_ -= amount;
  if (scrollOffset_ <= 0) {
    // Reached the bottom — release pin and re-enable auto-follow.
    scrollOffset_ = 0;
    autoScroll_ = true;
    userScrolledUp_ = false;
    pinnedTopLine_ = -1;
  } else if (userScrolledUp_ && pinnedTopLine_ >= 0) {
    // Update pinned position to match new offset.
    int totalLines = static_cast<int>(scrollback_.size());
    pinnedTopLine_ = totalLines - scrollOffset_;
  }
  markDirty();
}

void AppState::scrollToTop() {
  std::lock_guard<std::mutex> lock(mutex_);
  scrollOffset_ = static_cast<int>(scrollback_.size());
  autoScroll_ = false;
  userScrolledUp_ = true;
  pinnedTopLine_ = 0;  // Pin to the very top
  markDirty();
}

void AppState::scrollToBottom() {
  std::lock_guard<std::mutex> lock(mutex_);
  scrollOffset_ = 0;
  autoScroll_ = true;
  userScrolledUp_ = false;
  pinnedTopLine_ = -1;  // Release pin, re-enable auto-follow
  markDirty();
}

int AppState::scrollOffset() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return scrollOffset_;
}

bool AppState::isAtBottom() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return scrollOffset_ == 0;
}

void AppState::setAutoScroll(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  autoScroll_ = enabled;
  if (autoScroll_) scrollOffset_ = 0;
  markDirty();
}

} // namespace firmius::tui2
