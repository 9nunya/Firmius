#pragma once

#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <cstdint>

#include "HarnessEvents.hpp"
#include "Message.hpp"
#include "Context.hpp"
#include "Enums.hpp"
#include "ChatMessage.hpp"

namespace firmius::tui {

/**
 * @brief Thread-safe view model storing UI state for the TUI.
 */
class AppState {
public:
    AppState();
    ~AppState() = default;

    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;

    /**
     * @brief Apply a HarnessEvent to mutate internal state.
     * @param event The event to apply (std::variant of all event types).
     */
    void applyEvent(const firmius::harness::HarnessEvent& event);

    // Thread-safe getters
    std::vector<ChatMessage> getMessages() const;
    std::optional<ChatMessage> getStreamingMessage() const;
    // Public data structures returned by getters
    struct ToolCallInfo {
        std::string toolCallId;
        std::string agentId;
        std::string name;
        std::string args;
        std::string result;
        bool success = false;
    };

    struct SubagentInfo {
        std::string parentId;
        std::string agentId;
        std::string persona;
        std::string friendlyName;
        std::string title;
        std::string providerId;
        std::string modelId;
    };

    struct FooterInfo {
        std::string threadId;
        std::string threadTitle;
        firmius::shared::AgentStatus status = firmius::shared::AgentStatus::Idle;
        bool isCompacting = false;
        uint32_t tokensSaved = 0;
    };

    std::vector<SubagentInfo> getSubagents() const;
    FooterInfo getFooterInfo() const;
    std::unordered_set<std::string> getQueuedMessageTags() const;
    std::vector<std::string> getNotifications() const;
    std::vector<ToolCallInfo> getActiveToolCalls() const;
    std::unordered_map<std::string, firmius::shared::AgentMetrics> getMetrics() const;
    std::string getFocusedAgentId() const;
    void setFocusedAgentId(const std::string& agentId);

private:
    struct Notification {
        std::string text;
        uint64_t timestamp;
    };

    mutable std::recursive_mutex mutex_;

    // State
    std::string currentThreadId_;
    std::optional<firmius::shared::ThreadMetadata> currentThreadMetadata_;
    std::vector<ChatMessage> messages_;
    std::optional<ChatMessage> streamingMessage_;
    std::vector<SubagentInfo> subagents_;
    FooterInfo footer_;
    std::unordered_set<std::string> queuedMessageTags_;
    std::unordered_map<std::string, ToolCallInfo> activeToolCalls_;
    std::vector<Notification> notifications_;
    std::unordered_map<std::string, firmius::shared::AgentMetrics> metrics_;
    std::string focusedAgentId_;

    // Apply helpers for each event type
    void apply(const firmius::harness::ThreadChanged& e);
    void apply(const firmius::harness::MessageChunk& e);
    void apply(const firmius::harness::MessageCompleted& e);
    void apply(const firmius::harness::ToolCallStarted& e);
    void apply(const firmius::harness::ToolCallResult& e);
    void apply(const firmius::harness::SubagentSpawned& e);
    void apply(const firmius::harness::ProcessOutputChunk& e);
    void apply(const firmius::harness::ThreadLocked& e);
    void apply(const firmius::harness::HarnessError& e);
    void apply(const firmius::harness::AgentCompactingEvent& e);
    void apply(const firmius::harness::ContextCompactedEvent& e);
    void apply(const firmius::harness::ThreadDeleted& e);
    void apply(const firmius::harness::ConfigUpdated& e);
    void apply(const firmius::harness::ModelSwitchedEvent& e);
    void apply(const firmius::harness::HistoryUndoneEvent& e);
    void apply(const firmius::harness::ThreadTitleUpdated& e);
    void apply(const firmius::harness::MessageQueued& e);
    void apply(const firmius::harness::MessageDequeued& e);
    void apply(const firmius::harness::AgentRetrying& e);
    void apply(const firmius::harness::AgentRetryFailed& e);
    void apply(const firmius::harness::UserMessageSent& e);
    template<typename T> void apply(const T&) {}
};

} // namespace firmius::tui
