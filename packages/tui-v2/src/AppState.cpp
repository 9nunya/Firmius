#include "AppState.hpp"
#include "items/SimpleItems.hpp"
#include "items/ToolCallItem.hpp"
#include "items/StreamingItems.hpp"

#include <algorithm>
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

void AppState::setAgentContextUsage(ContextUsage usage) {
  std::lock_guard<std::mutex> lock(mutex_);
  agentContextUsage_ = usage;
  markDirty();
}

ContextUsage AppState::agentContextUsage() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return agentContextUsage_;
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

void AppState::setLiveMessage(const std::string &message) {
  std::lock_guard<std::mutex> lock(mutex_);
  liveMessage_ = message;
  markDirty();
}

std::string AppState::liveMessage() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return liveMessage_;
}

void AppState::setHookState(const firmius::daemon::HookStateSnapshot &hookState) {
  std::lock_guard<std::mutex> lock(mutex_);
  hookState_ = hookState;
  markDirty();
}

firmius::daemon::HookStateSnapshot AppState::hookState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return hookState_;
}

void AppState::setDaemonReady(bool ready) {
  std::lock_guard<std::mutex> lock(mutex_);
  daemonReady_ = ready;
  markDirty();
}

bool AppState::daemonReady() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return daemonReady_;
}

void AppState::resetWelcomeState() {
  std::lock_guard<std::mutex> lock(mutex_);
  threadId_.clear();
  threadTitle_.clear();
  agentId_.clear();
  focusedAgentId_.clear();
  primaryAgentId_.clear();
  hookState_ = {};
  agents_.clear();
  agentTodos_.clear();
  activeTextItem_ = nullptr;
  activeThinkingItem_ = nullptr;
  agentTextItems_.clear();
  agentThinkingItems_.clear();
  queuedMessageCount_ = 0;
  queuedMessageIds_.clear();
  queuedUserMessages_.clear();
  agentStatus_ = firmius::shared::AgentStatus::Idle;
  markDirty();
}

void AppState::prepareForThreadLoad() {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.clear();
  itemSpans_.clear();
  toolCalls_.clear();
  processToTool_.clear();
  agents_.clear();
  agentTodos_.clear();
  activeTextItem_ = nullptr;
  activeThinkingItem_ = nullptr;
  agentTextItems_.clear();
  agentThinkingItems_.clear();
  agentActivityLogs_.clear();
  focusedAgentId_.clear();
  primaryAgentId_.clear();
  hookState_ = {};
  scrollback_.clear();
  scrollOffset_ = 0;
  userScrolledUp_ = false;
  pinnedTopLine_ = -1;
  fullResyncRequested_ = true;
  queuedMessageCount_ = 0;
  queuedMessageIds_.clear();
  queuedUserMessages_.clear();
  agentStatus_ = firmius::shared::AgentStatus::Idle;
  markDirty();
}

// ── Items (transcript) ──

void AppState::addItem(std::unique_ptr<TranscriptItem> item) {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.push_back(std::move(item));
  markDirty();
}

void AppState::insertItem(size_t index, std::unique_ptr<TranscriptItem> item) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= items_.size()) {
    items_.push_back(std::move(item));
  } else {
    items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(index),
                  std::move(item));
  }
  fullResyncRequested_ = true;
  markDirty();
}

void AppState::clearTranscriptItems() {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.clear();
  toolCalls_.clear();
  processToTool_.clear();
  activeTextItem_ = nullptr;
  activeThinkingItem_ = nullptr;
  agentTextItems_.clear();
  agentThinkingItems_.clear();
  scrollback_.clear();
  scrollOffset_ = 0;
  userScrolledUp_ = false;
  pinnedTopLine_ = -1;
  fullResyncRequested_ = true;
  markDirty();
}

void AppState::clearItems() {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.clear();
  toolCalls_.clear();
  processToTool_.clear();
  agents_.clear();
  agentTodos_.clear();
  activeTextItem_ = nullptr;
  activeThinkingItem_ = nullptr;
  queuedMessageCount_ = 0;
  queuedMessageIds_.clear();
  queuedUserMessages_.clear();
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

UserMessageItem* AppState::findUserMessageById(const std::string& messageId) {
  if (messageId.empty()) {
    return nullptr;
  }
  for (auto& item : items_) {
    if (item->type() == "UserMessage") {
      auto* user = static_cast<UserMessageItem*>(item.get());
      if (user->messageId() == messageId) {
        return user;
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

std::unordered_map<std::string, AgentState>& AppState::agentsMut() {
  return agents_;
}

void AppState::renameAgent(const std::string& oldId, const std::string& newId) {
  auto it = agents_.find(oldId);
  if (it == agents_.end()) return;
  auto node = agents_.extract(oldId);
  node.key() = newId;
  agents_.insert(std::move(node));
  // Update parentId references for children
  for (auto& [id, agent] : agents_) {
    if (agent.parentId == oldId) agent.parentId = newId;
  }
}

void AppState::updateAgentModel(const std::string& agentId,
                                const std::string& providerId,
                                const std::string& modelId,
                                uint32_t contextWindowTokens) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& agent = agents_[agentId];
  agent.agentId = agentId;
  agent.providerId = providerId;
  agent.modelId = modelId;
  if (contextWindowTokens > 0) {
    agent.contextWindowTokens = contextWindowTokens;
  }
  agent.contextUsedTokens = 0;
  agent.contextSentTokens = 0;
  if (focusedAgentId_ == agentId || (focusedAgentId_.empty() && agentId_ == agentId)) {
    modelLabel_ = providerId.empty() ? modelId : providerId + "/" + modelId;
    agentContextUsage_ = ContextUsage{agent.contextWindowTokens, 0, 0};
    if (agent.contextWindowTokens > 0) {
      agentContextWindow_ = "0/" + std::to_string(agent.contextWindowTokens / 1000) + "k";
    } else {
      agentContextWindow_.clear();
    }
  }
  markDirty();
}

void AppState::clearAgentContextUsage(const std::string& agentId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = agents_.find(agentId);
  if (it != agents_.end()) {
    it->second.contextUsedTokens = 0;
    it->second.contextSentTokens = 0;
  }
  if (focusedAgentId_ == agentId || (focusedAgentId_.empty() && agentId_ == agentId)) {
    agentContextUsage_.usedTokens = 0;
    agentContextUsage_.sentTokens = 0;
    if (agentContextUsage_.windowTokens == 0) {
      agentContextWindow_.clear();
    } else {
      agentContextWindow_ =
          "0/" + std::to_string(agentContextUsage_.windowTokens / 1000) + "k";
    }
  }
  markDirty();
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
    agentContextUsage_ = ContextUsage{
        it->second.contextWindowTokens,
        it->second.contextUsedTokens,
        it->second.contextSentTokens,
    };
    if (it->second.contextWindowTokens > 0) {
      const uint32_t displayTokens = it->second.contextUsedTokens > 0
                                         ? it->second.contextUsedTokens
                                         : it->second.contextSentTokens;
      agentContextWindow_ = std::to_string(displayTokens / 1000) + "k/" +
                            std::to_string(it->second.contextWindowTokens / 1000) +
                            "k";
    } else {
      agentContextWindow_.clear();
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

std::vector<AgentState> AppState::agentList() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AgentState> result;
  result.reserve(agents_.size());
  for (auto& [id, agent] : agents_) {
    result.push_back(agent);
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
  if (threadId_.empty()) {
    return false;
  }
  return agents_.size() > 1;
}

std::string AppState::agentToolSummary(const std::string& agentId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> toolNames;
  for (const auto& item : items_) {
    if (item->type() == "ToolCall") {
      const auto* tc = static_cast<const ToolCallItem*>(item.get());
      if (tc->agentId() == agentId) {
        toolNames.push_back(tc->toolName());
      }
    }
  }
  if (toolNames.empty()) return {};
  // Show last 3 tool names
  std::string summary;
  size_t start = toolNames.size() > 3 ? toolNames.size() - 3 : 0;
  for (size_t i = start; i < toolNames.size(); ++i) {
    if (i > start) summary += ", ";
    summary += toolNames[i];
  }
  if (toolNames.size() > 3) summary = "..." + summary;
  return std::to_string(toolNames.size()) + " tools: " + summary;
}

void AppState::appendAgentActivity(const std::string& agentId, const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& log = agentActivityLogs_[agentId];
  log.push_back({"line:" + std::to_string(log.size()), line, {}});
  if (log.size() > 20) {
    log.erase(log.begin(), log.begin() + (log.size() - 20));
  }
}

void AppState::upsertAgentActivity(const std::string& agentId,
                                   const std::string& key,
                                   const std::string& line,
                                   std::chrono::milliseconds ttl) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& log = agentActivityLogs_[agentId];
  const auto expiry = ttl.count() > 0
                          ? std::chrono::steady_clock::now() + ttl
                          : std::chrono::steady_clock::time_point{};
  for (auto& entry : log) {
    if (entry.key == key) {
      entry.text = line;
      entry.expiresAt = expiry;
      return;
    }
  }
  log.push_back({key, line, expiry});
  if (log.size() > 20) {
    log.erase(log.begin(), log.begin() + (log.size() - 20));
  }
}

std::vector<std::string> AppState::agentActivityLog(const std::string& agentId, int maxLines) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = agentActivityLogs_.find(agentId);
  if (it == agentActivityLogs_.end()) return {};
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::string> live;
  for (const auto& entry : it->second) {
    if (entry.text.empty()) continue;
    if (entry.expiresAt != std::chrono::steady_clock::time_point{} &&
        now > entry.expiresAt) {
      continue;
    }
    live.push_back(entry.text);
  }
  int start = std::max(0, static_cast<int>(live.size()) - maxLines);
  return std::vector<std::string>(live.begin() + start, live.end());
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

AgentTextItem* AppState::agentTextItem(const std::string& agentId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = agentTextItems_.find(agentId);
  return (it != agentTextItems_.end()) ? it->second : nullptr;
}

void AppState::setAgentTextItem(const std::string& agentId, AgentTextItem* item) {
  std::lock_guard<std::mutex> lock(mutex_);
  agentTextItems_[agentId] = item;
}

AgentThinkingItem* AppState::agentThinkingItem(const std::string& agentId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = agentThinkingItems_.find(agentId);
  return (it != agentThinkingItems_.end()) ? it->second : nullptr;
}

void AppState::setAgentThinkingItem(const std::string& agentId, AgentThinkingItem* item) {
  std::lock_guard<std::mutex> lock(mutex_);
  agentThinkingItems_[agentId] = item;
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
  if (count <= 0) {
    queuedMessageIds_.clear();
    queuedUserMessages_.clear();
  }
  markDirty();
}

int AppState::queuedMessageCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queuedMessageCount_;
}

void AppState::queueMessageId(const std::string& messageId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!messageId.empty()) {
    queuedMessageIds_.insert(messageId);
    queuedMessageCount_ = static_cast<int>(queuedMessageIds_.size());
  } else {
    ++queuedMessageCount_;
  }
  markDirty();
}

void AppState::dequeueMessageId(const std::string& messageId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!messageId.empty()) {
    queuedMessageIds_.erase(messageId);
    queuedMessageCount_ = static_cast<int>(queuedMessageIds_.size());
  } else {
    queuedMessageCount_ = std::max(0, queuedMessageCount_ - 1);
  }
  markDirty();
}

bool AppState::isMessageQueued(const std::string& messageId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !messageId.empty() && queuedMessageIds_.count(messageId) > 0;
}

void AppState::upsertQueuedUserMessage(QueuedUserMessage message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (message.messageId.empty()) {
    return;
  }
  auto it = std::find_if(queuedUserMessages_.begin(), queuedUserMessages_.end(),
                         [&](const QueuedUserMessage& existing) {
                           return existing.messageId == message.messageId;
                         });
  if (it != queuedUserMessages_.end()) {
    *it = std::move(message);
  } else {
    queuedUserMessages_.push_back(std::move(message));
  }
  markDirty();
}

void AppState::removeQueuedUserMessage(const std::string& messageId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (messageId.empty()) {
    return;
  }
  queuedUserMessages_.erase(
      std::remove_if(queuedUserMessages_.begin(), queuedUserMessages_.end(),
                     [&](const QueuedUserMessage& message) {
                       return message.messageId == messageId;
                     }),
      queuedUserMessages_.end());
  markDirty();
}

std::vector<QueuedUserMessage>
AppState::queuedUserMessagesForAgent(const std::string& agentId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<QueuedUserMessage> result;
  for (const auto& message : queuedUserMessages_) {
    if (message.agentId == agentId) {
      result.push_back(message);
    }
  }
  return result;
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

// ── Embedding ──

void AppState::setEmbeddingDownload(EmbeddingDownloadState state) {
  std::lock_guard<std::mutex> lock(mutex_);
  embeddingDownload_ = std::move(state);
  markDirty();
}

EmbeddingDownloadState AppState::embeddingDownload() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return embeddingDownload_;
}

// ── Todos ──

void AppState::setAgentTodos(
    const std::string& agentId,
    const std::vector<firmius::shared::TodoItem>& items) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (agentId.empty()) {
    return;
  }
  agentTodos_[agentId] = items;
  markDirty();
}

std::vector<firmius::shared::TodoItem>
AppState::agentTodos(const std::string& agentId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = agentTodos_.find(agentId);
  if (it == agentTodos_.end()) {
    return {};
  }
  return it->second;
}

std::vector<firmius::shared::TodoItem> AppState::focusedAgentTodos() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string agentId = !focusedAgentId_.empty() ? focusedAgentId_ : agentId_;
  auto it = agentTodos_.find(agentId);
  if (it == agentTodos_.end()) {
    return {};
  }
  return it->second;
}

void AppState::clearTodos() {
  std::lock_guard<std::mutex> lock(mutex_);
  agentTodos_.clear();
  markDirty();
}

void AppState::toggleTodoVisibility() {
  std::lock_guard<std::mutex> lock(mutex_);
  todoVisible_ = !todoVisible_;
  markDirty();
}

bool AppState::todoVisible() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return todoVisible_;
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

bool AppState::consumeFullResyncRequested() {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool requested = fullResyncRequested_;
  fullResyncRequested_ = false;
  return requested;
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
