#pragma once

#include "Enums.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <optional>
#include <unordered_set>

#include "HarnessEvents.hpp"
#include "Context.hpp"
#include "Events.hpp"
#include "ConfigLoader.hpp"
#include "persistence/HistoryEditor.hpp"

namespace firmius::core {

using namespace firmius::shared;

class IHost;
class ThreadManager;

/**
 * Harness is a singleton layer that orchestrates thread management, PID-based locking,
 * and reactive event routing on top of Engine and AgentRegistry.
 */
class Harness {
public:
    // Singleton access
    static Harness& instance();

    // Prevent copying
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    // Prevent moving
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;

    /**
     * Initialize the Harness.
     * - Scans Docker for containers with com.firmius.owner_pid=<pid> labels
     * - Kills containers whose owning PID is dead
     * - Loads last_session.json to restore previous focus
     */
    void init();

    /**
     * Shutdown the Harness.
     * - Releases all flock handles for thread lock files
     * - Writes current session to ~/.firmius/last_session.json
     */
    void shutdown();

    /**
     * Create a new thread and establish PID lock.
     * @param hostType The type of host (Local, Docker)
     * @param cwd Working directory for the thread
     * @param leadPersona Persona name for the lead agent
     * @return The new thread ID, or empty string if locking failed
     */
    std::string newThread(shared::HostType hostType, const std::string& cwd, const std::string& leadPersona = "firmius");

    /**
     * Switch focus to a different thread.
     * @param threadId The thread ID to switch to
     * @return true if successful, false if thread is locked by another process
     */
    bool switchThread(const std::string& threadId);

    /**
     * Resume the last active session.
     * @return true if successfully resumed, false otherwise
     */
    bool resumeLast();

    /**
     * Send a message to the current lead agent.
     * @param text The message text to send
     */
    void send(const std::string& text);

    /**
     * Aborts the current agent's blocking process and interrupts the agent.
     * - Kills currentBlockingProcessId if set in OS
     * - Calls agent->interrupt()
     */
    void abort();

    /**
     * Subscribe to Harness events.
     * Events are routed only if agentId matches the focused agent or its descendants.
     * @param callback The callback function to invoke on events
     * @return A unique subscription ID for unsubscribing
     */
    int subscribe(std::function<void(const harness::HarnessEvent&)> callback);

    /**
     * Unsubscribe from Harness events.
     * @param subscriptionId The subscription ID returned from subscribe()
     */
    void unsubscribe(const int& subscriptionId);

    /**
     * Get the current thread ID.
     */
    std::string currentThreadId();

    /**
     * Get the focused agent ID for the current thread.
     */
    std::string focusedAgentId();

    void deleteThread(const std::string& threadId);

    std::vector<ThreadMetadata> listThreads();

    const UserConfig& getConfig();
    void updateConfig(const UserConfig& config);

    /**
     * Switches the model for the focused agent (idle-only).
     * @param providerId The new provider ID.
     * @param modelId The new model ID.
     */
    void switchModel(const std::string& providerId, const std::string& modelId);

    /**
     * Interrupts the focused agent and switches its model.
     * @param providerId The new provider ID.
     * @param modelId The new model ID.
     */
    void interruptAndSwitchModel(const std::string& providerId, const std::string& modelId);

    /**
     * Undoes the last N turns for the focused agent.
     * @param count Number of turns to undo.
     * @return Result of the undo operation.
     */
    UndoResult undoTurns(int count);

    /**
     * Undoes the last N messages for the focused agent.
     * @param count Number of messages to undo.
     * @return Result of the undo operation.
     */
    UndoResult undoMessages(int count);

    /**
     * Undoes all turns after a specific timestamp for the focused agent.
     * @param timestamp The timestamp threshold.
     * @return Result of the undo operation.
     */
    UndoResult undoAfterTimestamp(uint64_t timestamp);

    /**
     * Writes an interruption record to the current thread's journal directory.
     * Serializes in-flight tool calls and active subagents for crash recovery.
     */
    void writeInterruptionRecord();

private:
    Harness();
    ~Harness() = default;

    /**
     * Acquire an exclusive lock on a thread's lock file.
     * @param threadId The thread ID
     * @return File descriptor if successful, -1 if failed, -2 if would block
     */
    int acquireThreadLock(const std::string& threadId);

    /**
     * Release a thread lock.
     * @param threadId The thread ID
     */
    void releaseThreadLock(const std::string& threadId);

    /**
     * Check if an agent is a descendant of another agent.
     * @param agentId The agent ID to check
     * @param ancestorId The potential ancestor ID
     * @param depth Recursion depth for cycle detection
     * @return true if agentId is a descendant of ancestorId
     */
    bool isDescendant(const std::string& agentId, const std::string& ancestorId, int depth = 0);

    /**
     * Emit a HarnessEvent to all subscribers.
     * @param event The event to emit
     */
    void emitEvent(const harness::HarnessEvent& event);

    /**
     * Route an EngineEvent to HarnessEvent and emit if relevant.
     * @param event The EngineEvent to route
     */
    void routeEngineEvent(const shared::EngineEvent& event);

    // Current state
    std::string currentThreadId_;
    std::string focusedAgentId_;
    std::map<std::string, int> threadLocks_; // threadId -> file descriptor
    std::recursive_mutex mutex_;

    // Subscribers
    std::map<int, std::function<void(const harness::HarnessEvent&)>> subscribers_;
    int nextSubscriptionId_ = 0;

    // Agent state tracking for event routing
    std::map<std::string, std::string> agentParentMap_; // agentId -> parentId
    std::map<std::string, std::string> threadAgentMap_; // threadId -> focusedAgentId

    // Track which threads have had titles generated
    std::unordered_set<std::string> titleGeneratedThreads_;

    void maybeGenerateTitle(const std::string& threadId, const std::string& firstMessage);

    /**
     * Drains one message from the queue and sends it to the focused agent.
     * Called when agent turn completes.
     */
    void drainQueue();

    // Message queue for sending while agent is running
    std::queue<std::pair<std::string, std::string>> messageQueue_;  // id, text
};

} // namespace firmius::core
