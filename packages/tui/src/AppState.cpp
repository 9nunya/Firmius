#include "AppState.hpp"
#include "ChatMessage.hpp"

namespace firmius::tui {

using namespace firmius::harness;
using namespace firmius::shared;

AppState::AppState() {
    footer_.status = AgentStatus::Idle;
    footer_.isCompacting = false;
    footer_.tokensSaved = 0;
}

void AppState::applyEvent(const HarnessEvent& event) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::visit([this](auto& e) { apply(e); }, event);
}

// Apply implementations

void AppState::apply(const ThreadChanged& e) {
    currentThreadId_ = e.threadId;
    currentThreadMetadata_ = e.metadata;
    messages_.clear();
    streamingMessage_.reset();
    activeToolCalls_.clear();
    subagents_.clear();
    queuedMessageTags_.clear();
    notifications_.clear();
    metrics_.clear();

    footer_.threadId = e.threadId;
    footer_.threadTitle = e.metadata.title;
    footer_.status = AgentStatus::Idle;
    footer_.isCompacting = false;
    footer_.tokensSaved = 0;
}

void AppState::apply(const MessageChunk& e) {
    if (!streamingMessage_) {
        ChatMessage msg;
        msg.message.id = "stream-" + e.agentId;
        msg.message.role = Role::Assistant;
        msg.message.timestamp = 0;
        msg.agentId = e.agentId;
        streamingMessage_ = msg;
    }
    if (e.isThinking) {
        ThinkingContent thinking{e.delta};
        streamingMessage_->message.content.emplace_back(thinking);
    } else {
        TextContent text{e.delta};
        streamingMessage_->message.content.emplace_back(text);
    }
    footer_.status = AgentStatus::Streaming;
}

void AppState::apply(const MessageCompleted& e) {
    ChatMessage msg;
    msg.message = e.message;
    msg.agentId = e.agentId;
    msg.metrics = e.metrics;
    
    if (queuedMessageTags_.count(e.message.id)) {
        msg.type = ChatMessageType::Queued;
    }

    // Find friendly name
    for (const auto& sub : subagents_) {
        if (sub.agentId == e.agentId) {
            msg.friendlyName = sub.friendlyName;
            break;
        }
    }

    messages_.push_back(msg);
    metrics_[e.message.id] = e.metrics;
    streamingMessage_.reset();
    footer_.status = AgentStatus::Idle;
}

void AppState::apply(const ToolCallStarted& e) {
    ToolCallInfo info;
    info.toolCallId = e.toolCallId;
    info.agentId = e.agentId;
    info.name = e.name;
    info.args = e.args;
    info.result.clear();
    info.success = false;
    activeToolCalls_[e.toolCallId] = info;
    footer_.status = AgentStatus::ExecutingTool;
}

void AppState::apply(const ToolCallResult& e) {
    auto it = activeToolCalls_.find(e.toolCallId);
    if (it != activeToolCalls_.end()) {
        it->second.result = e.result;
        it->second.success = e.success;
        activeToolCalls_.erase(it);
    }
    if (activeToolCalls_.empty() && !streamingMessage_) {
        footer_.status = AgentStatus::Idle;
    }
}

void AppState::apply(const SubagentSpawned& e) {
    SubagentInfo info;
    info.parentId = e.parentId;
    info.agentId = e.agentId;
    info.persona = e.persona;
    info.friendlyName = e.friendlyName;
    info.title = e.title;
    info.providerId.clear();
    info.modelId.clear();
    subagents_.push_back(info);
}

void AppState::apply(const ProcessOutputChunk& e) {
    // Process output events not currently displayed in UI
    (void)e;
}

void AppState::apply(const ThreadLocked& e) {
    notifications_.push_back({"Thread " + e.threadId + " locked by PID " + std::to_string(e.ownerPid), 0});
    footer_.status = AgentStatus::Error;
}

void AppState::apply(const HarnessError& e) {
    notifications_.push_back({e.message, 0});
    footer_.status = AgentStatus::Error;
    
    ChatMessage msg;
    msg.type = ChatMessageType::Error;
    msg.specialText = e.message;
    messages_.push_back(msg);
}

void AppState::apply(const AgentCompactingEvent& e) {
    footer_.isCompacting = true;
    footer_.status = AgentStatus::Compacting;
    (void)e;
}

void AppState::apply(const ContextCompactedEvent& e) {
    footer_.isCompacting = false;
    footer_.tokensSaved = e.tokensSaved;
    footer_.status = AgentStatus::Idle;
    
    ChatMessage msg;
    msg.type = ChatMessageType::Compaction;
    msg.tokensSaved = e.tokensSaved;
    msg.agentId = e.agentId;
    messages_.push_back(msg);

    notifications_.push_back({"Context compacted: saved " + std::to_string(e.tokensSaved) + " tokens", 0});
}

void AppState::apply(const ThreadDeleted& e) {
    if (currentThreadId_ == e.threadId) {
        currentThreadId_.clear();
        currentThreadMetadata_.reset();
        messages_.clear();
        subagents_.clear();
        footer_.threadId.clear();
        footer_.threadTitle.clear();
        footer_.status = AgentStatus::Idle;
    }
}

void AppState::apply(const ConfigUpdated& e) {
    // Config updates not affecting UI state yet
    (void)e;
}

void AppState::apply(const ModelSwitchedEvent& e) {
    for (auto& sub : subagents_) {
        if (sub.agentId == e.agentId) {
            sub.providerId = e.newProviderId;
            sub.modelId = e.newModelId;
            break;
        }
    }
}

void AppState::apply(const HistoryUndoneEvent& e) {
    notifications_.push_back({"Undone " + std::to_string(e.turnsRemoved) + " turns", 0});
    if (e.compactionReversed) {
        notifications_.push_back({"Compaction reversed", 0});
    }
}

void AppState::apply(const ThreadTitleUpdated& e) {
    if (currentThreadId_ == e.threadId) {
        footer_.threadTitle = e.title;
        if (currentThreadMetadata_) {
            currentThreadMetadata_->title = e.title;
        }
    }
}

void AppState::apply(const MessageQueued& e) {
    queuedMessageTags_.insert(e.messageId);
    for (auto& msg : messages_) {
        if (msg.message.id == e.messageId) {
            msg.type = ChatMessageType::Queued;
            break;
        }
    }
}

void AppState::apply(const MessageDequeued& e) {
    queuedMessageTags_.erase(e.messageId);
    for (auto& msg : messages_) {
        if (msg.message.id == e.messageId) {
            msg.type = ChatMessageType::Normal;
            break;
        }
    }
}

void AppState::apply(const AgentRetrying& e) {
    std::string text = "Agent " + e.agentId + " retrying (attempt " + std::to_string(e.attempt) + "/" + std::to_string(e.maxAttempts) + ")";
    notifications_.push_back({text, 0});
    
    ChatMessage msg;
    msg.type = ChatMessageType::Retry;
    msg.attempt = e.attempt;
    msg.maxAttempts = e.maxAttempts;
    msg.delayMs = e.delayMs;
    msg.agentId = e.agentId;
    messages_.push_back(msg);
}

void AppState::apply(const AgentRetryFailed& e) {
    std::string text = "Agent " + e.agentId + " retry failed: " + e.reason;
    notifications_.push_back({text, 0});
    footer_.status = AgentStatus::Error;
}

void AppState::apply(const firmius::harness::UserMessageSent& e) {
    ChatMessage msg;
    msg.message.id = e.messageId;
    msg.message.role = Role::User;
    msg.message.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    msg.message.content.push_back(TextContent{e.text});
    msg.agentId = "user";
    messages_.push_back(msg);
}

// Getters

std::vector<ChatMessage> AppState::getMessages() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return messages_;
}

std::optional<ChatMessage> AppState::getStreamingMessage() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return streamingMessage_;
}

std::vector<AppState::SubagentInfo> AppState::getSubagents() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return subagents_;
}

AppState::FooterInfo AppState::getFooterInfo() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return footer_;
}

std::unordered_set<std::string> AppState::getQueuedMessageTags() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return queuedMessageTags_;
}

std::vector<std::string> AppState::getNotifications() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(notifications_.size());
    for (const auto& n : notifications_) {
        result.push_back(n.text);
    }
    return result;
}

std::vector<AppState::ToolCallInfo> AppState::getActiveToolCalls() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<ToolCallInfo> result;
    result.reserve(activeToolCalls_.size());
    for (const auto& [id, info] : activeToolCalls_) {
        result.push_back(info);
    }
    return result;
}

std::unordered_map<std::string, firmius::shared::AgentMetrics> AppState::getMetrics() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return metrics_;
}

std::string AppState::getFocusedAgentId() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return focusedAgentId_;
}

void AppState::setFocusedAgentId(const std::string& agentId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    focusedAgentId_ = agentId;
}

} // namespace firmius::tui
