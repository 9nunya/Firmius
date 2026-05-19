#include "AppState.hpp"
#include "items/SimpleItems.hpp"
#include "items/ToolCallItem.hpp"
#include "items/StreamingItems.hpp"
#include "items/QuickToolClusterItem.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_map>

namespace firmius::tui {

namespace {

// ── Placeholder span helpers ────────────────────────────────────────────
//
// Pasted blocks live in the input buffer as literal placeholder strings
// like "[Pasted #3: 14 lines]". They must behave as ATOMIC sequences:
//   * The cursor never lands inside one — caller asks for offset N, we
//     snap to either the start or the end of the surrounding span.
//   * Backspace at the trailing edge removes the whole span in one go.
//
// We don't store spans explicitly because the buffer is re-edited freely
// by the user — keeping a parallel index would invite drift. Instead we
// scan the buffer for "[Pasted #" markers on demand. The buffer is short
// (a chat input), so the linear scan is cheap.

struct PlaceholderSpan {
  size_t start = 0;   ///< Inclusive byte offset of the leading '['.
  size_t end = 0;     ///< Exclusive byte offset just past the trailing ']'.
  int id = 0;
};

std::string pluralize(const std::string& noun, int count) {
  return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

std::string joinPhrases(const std::vector<std::string>& parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += ", ";
    out += parts[i];
  }
  return out;
}

std::string buildTodoSummary(
    const std::vector<firmius::shared::TodoItem>& before,
    const std::vector<firmius::shared::TodoItem>& after) {
  using firmius::shared::TodoStatus;

  std::unordered_map<int, firmius::shared::TodoItem> beforeById;
  for (const auto& item : before) beforeById[item.id] = item;

  int added = 0;
  int removed = 0;
  int started = 0;
  int completed = 0;
  int reset = 0;

  for (const auto& item : after) {
    auto it = beforeById.find(item.id);
    if (it == beforeById.end()) {
      ++added;
      if (item.status == TodoStatus::InProgress) ++started;
      if (item.status == TodoStatus::Done) ++completed;
      continue;
    }

    if (it->second.status != item.status) {
      if (item.status == TodoStatus::InProgress) {
        ++started;
      } else if (item.status == TodoStatus::Done) {
        ++completed;
      } else if (item.status == TodoStatus::Pending) {
        ++reset;
      }
    }
    beforeById.erase(it);
  }

  removed = static_cast<int>(beforeById.size());

  std::vector<std::string> changes;
  if (added > 0) changes.push_back("added " + pluralize("item", added));
  if (completed > 0) changes.push_back("completed " + pluralize("item", completed));
  if (started > 0) changes.push_back("started " + pluralize("item", started));
  if (removed > 0) changes.push_back("removed " + pluralize("item", removed));
  if (reset > 0) changes.push_back("reset " + pluralize("item", reset));
  if (!changes.empty()) return joinPhrases(changes);

  int pending = 0;
  int inProgress = 0;
  int done = 0;
  for (const auto& item : after) {
    switch (item.status) {
    case TodoStatus::Pending:
      ++pending;
      break;
    case TodoStatus::InProgress:
      ++inProgress;
      break;
    case TodoStatus::Done:
      ++done;
      break;
    }
  }

  std::vector<std::string> counts;
  if (pending > 0) counts.push_back(pluralize("pending item", pending));
  if (inProgress > 0) counts.push_back(pluralize("active item", inProgress));
  if (done > 0) counts.push_back(pluralize("done item", done));
  if (!counts.empty()) return joinPhrases(counts);
  return "no active items";
}

// Scan the buffer and return all placeholder spans, in order of appearance.
std::vector<PlaceholderSpan> scanPlaceholders(const std::string& buf) {
  std::vector<PlaceholderSpan> spans;
  static constexpr const char* kMarker = "[Pasted #";
  size_t i = 0;
  while (i < buf.size()) {
    auto pos = buf.find(kMarker, i);
    if (pos == std::string::npos) break;
    size_t j = pos + 9;  // strlen("[Pasted #") == 9
    int id = 0;
    bool any = false;
    while (j < buf.size() &&
           std::isdigit(static_cast<unsigned char>(buf[j]))) {
      id = id * 10 + (buf[j] - '0');
      ++j;
      any = true;
    }
    if (!any || j >= buf.size() || buf[j] != ':') {
      i = pos + 1;
      continue;
    }
    auto closeBracket = buf.find(']', j);
    if (closeBracket == std::string::npos) break;
    PlaceholderSpan s;
    s.start = pos;
    s.end = closeBracket + 1;
    s.id = id;
    spans.push_back(s);
    i = s.end;
  }
  return spans;
}

// If `offset` lands inside any placeholder, return that span. Otherwise
// nullopt. Boundary positions (== start or == end) are NOT inside.
std::optional<PlaceholderSpan>
spanContaining(const std::string& buf, size_t offset) {
  for (const auto& s : scanPlaceholders(buf)) {
    if (offset > s.start && offset < s.end) return s;
  }
  return std::nullopt;
}

// Snap a desired offset to the nearest placeholder boundary if the
// movement direction implies one. `prefer` controls tie-breaking when
// the cursor is inside: PreferStart hops to the leading edge (used for
// leftward movement), PreferEnd hops to the trailing edge (rightward).
enum class SnapDir { PreferStart, PreferEnd };
size_t snapOutOfPlaceholder(const std::string& buf, size_t offset,
                             SnapDir prefer) {
  auto inside = spanContaining(buf, offset);
  if (!inside.has_value()) return offset;
  return prefer == SnapDir::PreferStart ? inside->start : inside->end;
}

// Find a placeholder whose end is at exactly `endExclusive`. Used by
// backspace to detect "cursor sits just past a placeholder".
std::optional<PlaceholderSpan>
spanEndingAt(const std::string& buf, size_t endExclusive) {
  for (const auto& s : scanPlaceholders(buf)) {
    if (s.end == endExclusive) return s;
  }
  return std::nullopt;
}

}  // namespace

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

void AppState::setPermissionMode(firmius::shared::ThreadPermissionMode mode) {
  std::lock_guard<std::mutex> lock(mutex_);
  permissionMode_ = mode;
  markDirty();
}

firmius::shared::ThreadPermissionMode AppState::permissionMode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return permissionMode_;
}

void AppState::setActiveModeId(std::string id) {
  std::lock_guard<std::mutex> lock(mutex_);
  activeModeId_ = std::move(id);
  markDirty();
}

std::string AppState::activeModeId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return activeModeId_;
}

void AppState::setModes(std::vector<ModeSummary> modes) {
  std::lock_guard<std::mutex> lock(mutex_);
  modes_ = std::move(modes);
  markDirty();
}

std::vector<AppState::ModeSummary> AppState::modes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return modes_;
}

std::string AppState::activeModeName() const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &m : modes_) {
    if (m.id == activeModeId_) return m.name;
  }
  return activeModeId_.empty() ? "ask" : activeModeId_;
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

void AppState::setMemoryStatus(MemoryStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  memoryStatus_ = status;
  markDirty();
}

MemoryStatus AppState::memoryStatus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return memoryStatus_;
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
  agentTodoSummaries_.clear();
  activeTextItem_ = nullptr;
  activeThinkingItem_ = nullptr;
  agentTextItems_.clear();
  agentThinkingItems_.clear();
  agentQuickToolClusterItems_.clear();
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
  agentTodoSummaries_.clear();
  activeTextItem_ = nullptr;
  activeThinkingItem_ = nullptr;
  agentTextItems_.clear();
  agentThinkingItems_.clear();
  agentQuickToolClusterItems_.clear();
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
  agentTodoSummaries_.clear();
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

QuickToolClusterItem* AppState::agentQuickToolClusterItem(const std::string& agentId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = agentQuickToolClusterItems_.find(agentId);
  return (it != agentQuickToolClusterItems_.end()) ? it->second : nullptr;
}

void AppState::setAgentQuickToolClusterItem(const std::string& agentId, QuickToolClusterItem* item) {
  std::lock_guard<std::mutex> lock(mutex_);
  agentQuickToolClusterItems_[agentId] = item;
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
    } else if (item->type() == "AgentThinking") {
      auto* think = static_cast<const AgentThinkingItem*>(item.get());
      if (think->needsAnimationTick()) return true;
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
    } else if (item->type() == "AgentThinking") {
      auto* think = static_cast<AgentThinkingItem*>(item.get());
      if (think->needsAnimationTick()) {
        think->markDirty();
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

std::vector<PendingPermission> AppState::pendingPermissions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::vector<PendingPermission>(pendingPermissions_.begin(),
                                         pendingPermissions_.end());
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
  const auto previous = agentTodos_[agentId];
  agentTodoSummaries_[agentId] = buildTodoSummary(previous, items);
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

std::string AppState::focusedAgentTodoSummary() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string agentId = !focusedAgentId_.empty() ? focusedAgentId_ : agentId_;
  auto it = agentTodoSummaries_.find(agentId);
  if (it == agentTodoSummaries_.end()) {
    return "";
  }
  return it->second;
}

void AppState::clearTodos() {
  std::lock_guard<std::mutex> lock(mutex_);
  agentTodos_.clear();
  agentTodoSummaries_.clear();
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
  // Move cursor to the end of the new buffer. That's the natural "I just
  // wrote text here" semantic (matches most editors on programmatic sets,
  // and matches the previous append-only behaviour these methods had
  // before we introduced explicit cursor tracking).
  inputCursor_ = inputBuffer_.size();
  inputDesiredColumn_ = -1;
  markDirty();
}

std::string AppState::inputBuffer() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return inputBuffer_;
}

void AppState::appendToInput(char ch) {
  std::lock_guard<std::mutex> lock(mutex_);
  inputBuffer_.insert(inputCursor_, 1, ch);
  ++inputCursor_;
  inputDesiredColumn_ = -1;
  markDirty();
}

void AppState::insertAtCursor(const std::string &text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  inputBuffer_.insert(inputCursor_, text);
  inputCursor_ += text.size();
  inputDesiredColumn_ = -1;
  markDirty();
}

void AppState::backspaceInput() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (inputCursor_ == 0 || inputBuffer_.empty()) return;
  // If the cursor sits immediately after a placeholder, eat the WHOLE
  // span in one keystroke — placeholders are atomic. We also drop the
  // matching block from pastedBlocks_ so submit doesn't try to inline
  // a deleted attachment.
  if (auto span = spanEndingAt(inputBuffer_, inputCursor_)) {
    inputBuffer_.erase(span->start, span->end - span->start);
    inputCursor_ = span->start;
    inputDesiredColumn_ = -1;
    for (auto it = pastedBlocks_.begin(); it != pastedBlocks_.end(); ++it) {
      if (it->id == span->id) {
        pastedBlocks_.erase(it);
        break;
      }
    }
    markDirty();
    return;
  }
  // Erase the byte BEFORE the cursor (not the byte at the cursor — that's
  // what "Delete" would do, which we don't bind here).
  inputBuffer_.erase(inputCursor_ - 1, 1);
  --inputCursor_;
  inputDesiredColumn_ = -1;
  markDirty();
}

namespace {

// Word-class helpers for word-wise navigation. We treat an alpha+digit run
// (including '_') as a word, and any other run of non-whitespace as its
// own word boundary. This is roughly emacs M-b/M-f and matches most input
// fields.
bool isWordChar(unsigned char c) {
  return std::isalnum(c) || c == '_';
}

}  // namespace

size_t AppState::deleteWordBeforeCursor() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (inputCursor_ == 0) return 0;
  size_t end = inputCursor_;
  // Skip trailing whitespace before the cursor.
  while (end > 0 && std::isspace(static_cast<unsigned char>(
                       inputBuffer_[end - 1]))) {
    --end;
  }
  // Now skip the word characters.
  while (end > 0 && isWordChar(static_cast<unsigned char>(
                       inputBuffer_[end - 1]))) {
    --end;
  }
  // If we didn't move at all (cursor was at a punctuation byte), eat a
  // single byte so the user makes some progress.
  if (end == inputCursor_) {
    end = inputCursor_ - 1;
  }
  size_t removed = inputCursor_ - end;
  inputBuffer_.erase(end, removed);
  inputCursor_ = end;
  inputDesiredColumn_ = -1;
  markDirty();
  return removed;
}

void AppState::clearInput() {
  std::lock_guard<std::mutex> lock(mutex_);
  inputBuffer_.clear();
  inputCursor_ = 0;
  inputDesiredColumn_ = -1;
  // Block storage is keyed to placeholders that no longer exist; drop it
  // so a stale image doesn't ride along on the next /command.
  pastedBlocks_.clear();
  markDirty();
}

size_t AppState::cursorOffset() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return inputCursor_;
}

void AppState::setCursorOffset(size_t offset) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (offset > inputBuffer_.size()) offset = inputBuffer_.size();
  // Programmatic seeks (mouse clicks etc.) snap to the trailing edge of
  // any placeholder they land inside — that matches "click here" intuition.
  inputCursor_ = snapOutOfPlaceholder(inputBuffer_, offset, SnapDir::PreferEnd);
  inputDesiredColumn_ = -1;
  markDirty();
}

void AppState::moveCursorLeft() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (inputCursor_ == 0) return;
  size_t target = inputCursor_ - 1;
  // If walking left lands inside a placeholder, jump past its leading
  // edge — one keystroke = one atom.
  if (auto inside = spanContaining(inputBuffer_, target)) {
    target = inside->start;
  }
  inputCursor_ = target;
  inputDesiredColumn_ = -1;
  markDirty();
}

void AppState::moveCursorRight() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (inputCursor_ >= inputBuffer_.size()) return;
  size_t target = inputCursor_ + 1;
  if (auto inside = spanContaining(inputBuffer_, target)) {
    target = inside->end;
  }
  inputCursor_ = target;
  inputDesiredColumn_ = -1;
  markDirty();
}

namespace {

// Find (line index, column offset) for a byte offset.
struct LinePos { int line; int column; };
LinePos lineColForOffset(const std::string& buf, size_t offset) {
  int line = 0;
  size_t lineStart = 0;
  for (size_t i = 0; i < offset && i < buf.size(); ++i) {
    if (buf[i] == '\n') {
      ++line;
      lineStart = i + 1;
    }
  }
  return {line, static_cast<int>(offset - lineStart)};
}

// Find byte offset for (line index, column).
size_t offsetForLineCol(const std::string& buf, int line, int col) {
  int curLine = 0;
  size_t i = 0;
  // Walk to the start of the requested line.
  while (i < buf.size() && curLine < line) {
    if (buf[i] == '\n') ++curLine;
    ++i;
  }
  if (curLine < line) return buf.size();  // line out of range
  // Now walk up to `col` chars (or end-of-line).
  size_t offset = i;
  while (offset < buf.size() && buf[offset] != '\n' &&
         static_cast<int>(offset - i) < col) {
    ++offset;
  }
  return offset;
}

}  // namespace

int AppState::cursorLineIndex() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lineColForOffset(inputBuffer_, inputCursor_).line;
}

int AppState::cursorColumnOnLine() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lineColForOffset(inputBuffer_, inputCursor_).column;
}

int AppState::inputLineCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (inputBuffer_.empty()) return 1;
  int n = 1;
  for (char c : inputBuffer_) if (c == '\n') ++n;
  return n;
}

std::string AppState::inputLineAt(int index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  int line = 0;
  size_t lineStart = 0;
  for (size_t i = 0; i <= inputBuffer_.size(); ++i) {
    if (i == inputBuffer_.size() || inputBuffer_[i] == '\n') {
      if (line == index) {
        return inputBuffer_.substr(lineStart, i - lineStart);
      }
      ++line;
      lineStart = i + 1;
    }
  }
  return {};
}

void AppState::moveCursorUp() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto pos = lineColForOffset(inputBuffer_, inputCursor_);
  if (pos.line == 0) return;  // at the top — caller can decide what to do
  // Remember the column we were on so consecutive Up/Down doesn't drift.
  if (inputDesiredColumn_ < 0) inputDesiredColumn_ = pos.column;
  size_t target =
      offsetForLineCol(inputBuffer_, pos.line - 1, inputDesiredColumn_);
  // Up/Down can land inside a placeholder; snap to the leading edge so
  // the cursor visibly stays on the line above the placeholder.
  inputCursor_ = snapOutOfPlaceholder(inputBuffer_, target, SnapDir::PreferStart);
  markDirty();
}

void AppState::moveCursorDown() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto pos = lineColForOffset(inputBuffer_, inputCursor_);
  // Count total lines.
  int total = 1;
  for (char c : inputBuffer_) if (c == '\n') ++total;
  if (pos.line >= total - 1) return;
  if (inputDesiredColumn_ < 0) inputDesiredColumn_ = pos.column;
  size_t target =
      offsetForLineCol(inputBuffer_, pos.line + 1, inputDesiredColumn_);
  inputCursor_ = snapOutOfPlaceholder(inputBuffer_, target, SnapDir::PreferEnd);
  markDirty();
}

void AppState::moveCursorWordLeft() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (inputCursor_ == 0) return;
  size_t i = inputCursor_;
  // Skip whitespace.
  while (i > 0 && std::isspace(static_cast<unsigned char>(inputBuffer_[i - 1]))) {
    --i;
  }
  // Skip a run (word chars, or a single non-word punctuation byte).
  if (i > 0 && isWordChar(static_cast<unsigned char>(inputBuffer_[i - 1]))) {
    while (i > 0 && isWordChar(static_cast<unsigned char>(inputBuffer_[i - 1]))) {
      --i;
    }
  } else if (i > 0) {
    --i;
  }
  inputCursor_ = snapOutOfPlaceholder(inputBuffer_, i, SnapDir::PreferStart);
  inputDesiredColumn_ = -1;
  markDirty();
}

void AppState::moveCursorWordRight() {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t i = inputCursor_;
  size_t n = inputBuffer_.size();
  // Skip whitespace.
  while (i < n && std::isspace(static_cast<unsigned char>(inputBuffer_[i]))) {
    ++i;
  }
  // Skip a word run (or one non-word byte).
  if (i < n && isWordChar(static_cast<unsigned char>(inputBuffer_[i]))) {
    while (i < n && isWordChar(static_cast<unsigned char>(inputBuffer_[i]))) {
      ++i;
    }
  } else if (i < n) {
    ++i;
  }
  inputCursor_ = snapOutOfPlaceholder(inputBuffer_, i, SnapDir::PreferEnd);
  inputDesiredColumn_ = -1;
  markDirty();
}

void AppState::moveCursorLineStart() {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t i = inputCursor_;
  while (i > 0 && inputBuffer_[i - 1] != '\n') --i;
  inputCursor_ = snapOutOfPlaceholder(inputBuffer_, i, SnapDir::PreferStart);
  inputDesiredColumn_ = -1;
  markDirty();
}

void AppState::moveCursorLineEnd() {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t i = inputCursor_;
  size_t n = inputBuffer_.size();
  while (i < n && inputBuffer_[i] != '\n') ++i;
  inputCursor_ = snapOutOfPlaceholder(inputBuffer_, i, SnapDir::PreferEnd);
  inputDesiredColumn_ = -1;
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

// ── Selection ────────────────────────────────────────────────────────────

namespace {

// Strip ANSI CSI sequences from a single line. We have a more general
// version in Terminal::ansi but keeping a tight local one means selection
// extraction doesn't have to drag in that header.
std::string stripAnsiLine(const std::string &line) {
  std::string out;
  out.reserve(line.size());
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
      // Skip until final byte (0x40-0x7E).
      size_t j = i + 2;
      while (j < line.size()) {
        unsigned char c = static_cast<unsigned char>(line[j]);
        if (c >= 0x40 && c <= 0x7E) {
          ++j;
          break;
        }
        ++j;
      }
      i = j - 1;
      continue;
    }
    out += line[i];
  }
  return out;
}

}  // namespace

void AppState::beginSelection(int absLine, int col) {
  std::lock_guard<std::mutex> lock(mutex_);
  selectionAnchor_ = {absLine, col};
  selectionCursor_ = {absLine, col};
  selectionActive_ = true;
  markDirty();
}

void AppState::updateSelection(int absLine, int col) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (selectionAnchor_.line < 0) return;
  selectionCursor_ = {absLine, col};
  markDirty();
}

void AppState::endSelection() {
  std::lock_guard<std::mutex> lock(mutex_);
  selectionActive_ = false;
  // Keep the highlighted range visible until the user clears or copies.
  // If the anchor and cursor land on the exact same point, treat as no
  // selection so a stray click doesn't leave a flicker.
  if (selectionAnchor_ == selectionCursor_) {
    selectionAnchor_ = {};
    selectionCursor_ = {};
  }
  markDirty();
}

void AppState::clearSelection() {
  std::lock_guard<std::mutex> lock(mutex_);
  selectionAnchor_ = {};
  selectionCursor_ = {};
  selectionActive_ = false;
  markDirty();
}

bool AppState::hasSelection() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return selectionAnchor_.line >= 0 && selectionCursor_.line >= 0 &&
         !(selectionAnchor_ == selectionCursor_);
}

bool AppState::isSelecting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return selectionActive_;
}

std::pair<AppState::SelectionPoint, AppState::SelectionPoint>
AppState::selectionRange() const {
  std::lock_guard<std::mutex> lock(mutex_);
  SelectionPoint a = selectionAnchor_;
  SelectionPoint b = selectionCursor_;
  if (a.line < 0 || b.line < 0) return {{}, {}};
  // Order so that `a` <= `b` in reading order.
  if (a.line > b.line || (a.line == b.line && a.col > b.col)) {
    std::swap(a, b);
  }
  return {a, b};
}

std::string AppState::copySelectedText() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (selectionAnchor_.line < 0 || selectionCursor_.line < 0 ||
      selectionAnchor_ == selectionCursor_) {
    return "";
  }
  SelectionPoint a = selectionAnchor_;
  SelectionPoint b = selectionCursor_;
  if (a.line > b.line || (a.line == b.line && a.col > b.col)) {
    std::swap(a, b);
  }
  std::string out;
  for (int line = a.line; line <= b.line; ++line) {
    if (line < 0 || line >= static_cast<int>(scrollback_.size())) continue;
    std::string clean = stripAnsiLine(scrollback_[static_cast<size_t>(line)]);
    int from = (line == a.line) ? a.col : 0;
    int to = (line == b.line) ? b.col : static_cast<int>(clean.size());
    if (from < 0) from = 0;
    if (to > static_cast<int>(clean.size())) to = static_cast<int>(clean.size());
    if (from < to) {
      out += clean.substr(static_cast<size_t>(from), static_cast<size_t>(to - from));
    }
    if (line < b.line) out += '\n';
  }
  return out;
}

// ── Pasted blocks ────────────────────────────────────────────────────────

namespace {

// Buffer-side placeholder format. The "#N" makes it unique so backspace can
// pinpoint exactly which block it sits next to, even if the user retyped
// the same friendly text by hand. The whole atom is one logical sequence
// of bytes — backspace removes the WHOLE thing in a single keystroke.
std::string makeTextPlaceholder(int id, int lineCount) {
  return "[Pasted #" + std::to_string(id) + ": " +
         std::to_string(lineCount) +
         (lineCount == 1 ? " line]" : " lines]");
}
std::string makeImagePlaceholder(int id) {
  return "[Pasted #" + std::to_string(id) + ": image]";
}

int countLines(const std::string& s) {
  if (s.empty()) return 0;
  int n = 1;
  for (char c : s) {
    if (c == '\n') ++n;
  }
  return n;
}

// Scan for a placeholder ending at the given offset. If one is found,
// return its start offset and parse the id; otherwise return nullopt.
struct PlaceholderAt {
  size_t start = 0;
  size_t end = 0;
  int id = 0;
};
std::optional<PlaceholderAt>
findPlaceholderEndingAt(const std::string& buf, size_t endExclusive) {
  if (endExclusive == 0 || endExclusive > buf.size()) return std::nullopt;
  if (buf[endExclusive - 1] != ']') return std::nullopt;
  // Walk backward looking for "[Pasted #".
  // Bound the scan — the placeholder is short, no point looking 1KB back.
  const size_t maxScan = std::min<size_t>(endExclusive, 64);
  for (size_t back = 1; back <= maxScan; ++back) {
    const size_t i = endExclusive - back;
    if (i + 9 > buf.size()) continue;
    if (buf.compare(i, 9, "[Pasted #") != 0) continue;
    // Parse digits after "[Pasted #".
    size_t j = i + 9;
    int id = 0;
    bool any = false;
    while (j < endExclusive - 1 && std::isdigit(static_cast<unsigned char>(buf[j]))) {
      id = id * 10 + (buf[j] - '0');
      ++j;
      any = true;
    }
    if (!any) continue;
    if (j >= endExclusive - 1 || buf[j] != ':') continue;
    PlaceholderAt p;
    p.start = i;
    p.end = endExclusive;
    p.id = id;
    return p;
  }
  return std::nullopt;
}

}  // namespace

std::string AppState::insertPastedText(std::string content) {
  std::lock_guard<std::mutex> lock(mutex_);
  PastedBlock block;
  block.id = nextPastedBlockId_++;
  block.kind = PastedBlockKind::Text;
  block.lineCount = countLines(content);
  block.content = std::move(content);
  const std::string placeholder =
      makeTextPlaceholder(block.id, block.lineCount);
  pastedBlocks_.push_back(std::move(block));
  inputBuffer_.insert(inputCursor_, placeholder);
  inputCursor_ += placeholder.size();
  inputDesiredColumn_ = -1;
  markDirty();
  return placeholder;
}

std::string AppState::insertPastedImage(std::string base64,
                                         std::string mediaType) {
  std::lock_guard<std::mutex> lock(mutex_);
  PastedBlock block;
  block.id = nextPastedBlockId_++;
  block.kind = PastedBlockKind::Image;
  block.content = std::move(base64);
  block.mediaType = std::move(mediaType);
  const std::string placeholder = makeImagePlaceholder(block.id);
  pastedBlocks_.push_back(std::move(block));
  inputBuffer_.insert(inputCursor_, placeholder);
  inputCursor_ += placeholder.size();
  inputDesiredColumn_ = -1;
  markDirty();
  return placeholder;
}

std::vector<AppState::PastedBlock> AppState::pastedBlocks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pastedBlocks_;
}

std::vector<AppState::PastedBlock> AppState::takePastedBlocks() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto out = std::move(pastedBlocks_);
  pastedBlocks_.clear();
  return out;
}

bool AppState::maybeBackspacePastedBlock() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = findPlaceholderEndingAt(inputBuffer_, inputCursor_);
  if (!found.has_value()) return false;
  // Erase the placeholder atomically.
  inputBuffer_.erase(found->start, found->end - found->start);
  inputCursor_ = found->start;
  inputDesiredColumn_ = -1;
  // Drop the matching block. Multiple blocks might share an id only if
  // the user did something weird; we drop the first match in registration
  // order, which matches the buffer's leftmost occurrence as we just
  // erased its slot.
  for (auto it = pastedBlocks_.begin(); it != pastedBlocks_.end(); ++it) {
    if (it->id == found->id) {
      pastedBlocks_.erase(it);
      break;
    }
  }
  markDirty();
  return true;
}

}  // namespace firmius::tui
