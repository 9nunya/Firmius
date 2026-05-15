#pragma once

#include "Enums.hpp"
#include <functional>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include "utils/FastHash.hpp"
#include <vector>

#include "ConfigLoader.hpp"
#include "Context.hpp"
#include "Events.hpp"
#include "IAgent.hpp"
#include "IHost.hpp"
#include "harness/ThreadLockManager.hpp"
#include "persistence/HistoryEditor.hpp"
#include "persistence/ThreadManager.hpp"

namespace firmius::core {

using namespace firmius::shared;

/**
 * Harness is a singleton layer that orchestrates thread management, PID-based
 * locking, and reactive event routing on top of Engine and AgentRegistry.
 */
class Harness {
public:
  // Singleton access
  static Harness &instance();

  // Prevent copying
  Harness(const Harness &) = delete;
  Harness &operator=(const Harness &) = delete;

  // Prevent moving
  Harness(Harness &&) = delete;
  Harness &operator=(Harness &&) = delete;

  // Debug logging flag for verbose turn/tool/thinking output
  bool debugLogging = false;

  // Internal state for debug logging tool call lifecycle
  enum class DebugToolPhase { Preparing, Called, Finished };
  struct DebugToolState {
    std::string name;
    std::string args;
    std::string result;
    DebugToolPhase phase = DebugToolPhase::Preparing;
    bool success = false;
    std::string agentId;
  };
  firmius::shared::utils::FastHash<std::string, DebugToolState> debugToolStates_; // toolCallId -> state
  firmius::shared::utils::FastHash<std::string, std::string> debugAgentToolMap_; // agentId -> current toolCallId
  uint64_t lastTurnCompletionTime_ =
      0; // Track last turn completion time to avoid duplicates

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
   * @param hostOptions Options for host creation (Type, Image, etc.)
   * @param cwd Working directory for the thread
   * @param leadPersona Persona name for the lead agent
   * @return The new thread ID, or empty string if locking failed
   */
  std::string newThread(shared::HostCreationOptions hostOptions,
                        const std::string &cwd,
                        const std::string &leadPersona = "",
                        const std::string &initialMode = "");

  /**
   * Switch focus to a different thread.
   * @param threadId The thread ID to switch to
   * @return true if successful, false if thread is locked by another process
   */
  bool switchThread(const std::string &threadId);

  /**
   * Resume the last active session.
   * @return true if successfully resumed, false otherwise
   */
  bool resumeLast();

  /**
   * Send a message to the current lead agent.
   * @param text The message text to send
   * @param images Optional vector of image content to include with the message
   */
  void send(const std::string &text,
            const std::vector<firmius::shared::ImageContent> &images = {});
  bool sendToThreadAgent(
      const std::string &threadId, const std::string &agentId,
      const std::string &text,
      const std::vector<firmius::shared::ImageContent> &images = {});
  bool retryLastRequest(std::string &statusMessage);

  /**
   * Execute a workflow by ID with the provided arguments.
   * Loads the workflow, replaces $1, $2, etc. placeholders with args,
   * and sends the resulting prompt to the focused agent.
   * @param workflowId The workflow ID (filename without extension).
   * @param args Vector of argument values to substitute.
   * @return true if the workflow was executed, false if workflow not found.
   */
  bool executeWorkflow(const std::string &workflowId,
                       const std::vector<std::string> &args);

  /**
   * Aborts the current agent's blocking process and interrupts the agent.
   * - Kills currentBlockingProcessId if set in OS
   * - Calls agent->interrupt()
   */
  void abort();

  /**
   * Interrupts the focused agent and preserves queued messages targeting it.
   * If queued messages exist for the focused running agent, this waits for the
   * current run to actually settle and then flushes the queued batch
   * immediately.
   */
  void abortAndFlushQueuedMessages();

  /**
   * Subscribe to Harness events.
   * Events are routed only if agentId matches the focused agent or its
   * descendants.
   * @param callback The callback function to invoke on events
   * @return A unique subscription ID for unsubscribing
   */
  int subscribe(
      std::function<void(const firmius::shared::AppEvent &)> callback);

  /**
   * Unsubscribe from Harness events.
   * @param subscriptionId The subscription ID returned from subscribe()
   */
  void unsubscribe(const int &subscriptionId);

  /**
   * Publish an app event to Harness subscribers.
   */
  void publishEvent(const firmius::shared::AppEvent &event);

  /**
   * Get the current thread ID.
   */
  std::string currentThreadId();

  /**
   * Get the focused agent ID for the current thread.
   */
  std::string focusedAgentId();

  /**
   * Change the focused agent for the current thread.
   */
  bool setFocusedAgent(const std::string &agentId);

  /**
   * Switches the lead persona for the current thread and focuses a new lead
   * agent if possible.
   * @param personaName The new lead persona name.
   * @return true if the switch succeeded, false otherwise.
   */
  bool switchLeadPersona(const std::string &personaName);
  ThreadPermissionMode threadPermissionMode(const std::string &threadId);
  ThreadPermissionRules threadPermissionRules(const std::string &threadId);
  bool commandMatchesPersistedAllowRule(const std::string &threadId,
                                        const std::string &command,
                                        const std::string &toolName = "");
  bool pathMatchesPersistedAllowRule(const std::string &threadId,
                                     const std::string &absolutePath,
                                     bool readOnly,
                                     const std::string &toolName = "");
  bool toolHasSessionAllowance(const std::string &threadId,
                               const std::string &toolName);
  bool readHasSessionAllowance(const std::string &threadId);
  void persistCommandAllowRule(const std::string &threadId,
                               const CommandAllowRule &rule);
  void persistPathAllowRule(const std::string &threadId,
                            const PathAllowRule &rule);
  void persistToolSessionAllowance(const std::string &threadId,
                                   const std::string &toolName);
  void persistReadSessionAllowance(const std::string &threadId);
  ThreadPermissionMode currentThreadPermissionMode();
  bool setCurrentThreadPermissionMode(ThreadPermissionMode mode);
  bool setThreadPermissionMode(const std::string &threadId,
                               ThreadPermissionMode mode);
  std::optional<ThreadPermissionMode> cycleCurrentThreadPermissionMode();
  PermissionResponse
  requestPermissionEscalation(PermissionEscalationRequest request);
  bool resolvePermissionEscalation(const std::string &requestId,
                                   PermissionResponse response);
  std::vector<PermissionEscalationRequest>
  listPendingPermissionEscalations(const std::string &threadId = "");
  bool markThreadAsBenchmark(const std::string &threadId,
                             const std::string &benchmarkId,
                             const std::string &benchmarkTaskId = "");
  bool appendSystemMessage(
      const std::string &agentId, const std::string &text,
      MessageVisibility visibility = MessageVisibility::Visible);

  void deleteThread(const std::string &threadId);

  std::vector<ThreadMetadata> listThreads();
  // Fast-path single-thread metadata lookup. listThreads() loads EVERY
  // thread in the DB (can be thousands) just so callers can find one entry
  // by id — that full-list read was the source of the observable freeze
  // on the welcome→chat transition, where submitPrompt iterated the whole
  // list to rebind metadata for the just-created thread.
  ThreadMetadata getThreadMetadata(const std::string &threadId);
  std::vector<std::string> listAgents(const std::string &threadId = "");
  std::vector<shared::ThreadArtifactMetadata>
  listArtifacts(const std::string &threadId = "");
  std::vector<shared::ModelInfo> listAllModels();
  std::vector<shared::ModelInfo> cachedModelsSnapshot() const;
  bool isModelsLoaded() const;
  std::vector<std::string> listProvidersFetchingModels() const;
  void invalidateModelCache();

  const UserConfig &getConfig() const;
  void updateConfig(const UserConfig &config);
  void saveConfig();

  std::vector<shared::OAuthAccount> getAccounts(const std::string &providerId);
  void deleteAccount(const std::string &providerId,
                     const std::string &identifier);
  std::map<std::string, std::vector<shared::QuotaBucket>>
  getAllQuotas(const std::string &providerId);
  std::map<std::string, std::vector<shared::QuotaBucket>>
  getCachedAllQuotas(const std::string &providerId);

  /**
   * Switches the model for the focused agent.
   * Applies immediately when idle, or queues for the next turn when running.
   * @param providerId The new provider ID.
   * @param modelId The new model ID.
   */
  void switchModel(const std::string &providerId, const std::string &modelId);

  /**
   * Switches the model for the focused agent with a specific variant
   * Applies immediately when idle, or queues for the next turn when running.
   * @param providerId The new provider ID.
   * @param modelId The new model ID.
   * @param variantName The model variant name (e.g., "low", "medium", "max").
   */
  void switchModel(const std::string &providerId, const std::string &modelId,
                   const std::string &variantName);

  /**
   * Interrupts the focused agent and switches its model.
   * @param providerId The new provider ID.
   * @param modelId The new model ID.
   */
  void interruptAndSwitchModel(const std::string &providerId,
                               const std::string &modelId);

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
   * Persisted transcript undo/redo surfaces with redo payload capture.
   */
  std::optional<shared::TranscriptUndoAction> undoTurnsWithRedo(int count);
  std::optional<shared::TranscriptUndoAction> undoMessagesWithRedo(int count);
  std::optional<shared::TranscriptUndoAction> undoAfterTimestampWithRedo(uint64_t timestamp);
  shared::TranscriptRedoEligibility evaluateTranscriptRedo(const std::string &undoActionId);
  std::optional<shared::TranscriptRedoAction> redoTranscriptUndoAction(const std::string &undoActionId);

  /**
   * Lists persisted edit batches for the current or specified thread.
   */
  std::vector<shared::EditBatchSummary>
  listEditBatches(const std::string &threadId = "",
                  const shared::EditHistoryFilters &filters = {});
  /** Evaluates whether an edit batch can be safely undone right now. */
  shared::EditUndoEligibility evaluateEditBatchUndo(const std::string &editBatchId);
  /** Attempts to undo a persisted edit batch for the focused agent. */
  std::optional<shared::EditUndoAction>
  undoEditBatch(const std::string &editBatchId);
  shared::EditRedoEligibility evaluateEditBatchRedo(const std::string &undoActionId);
  std::optional<shared::EditRedoAction>
  redoEditUndoAction(const std::string &undoActionId);

  /**
   * Compact the focused agent's context without resuming execution.
   */
  void compactFocusedAgent();

  /**
   * @brief Writes an interruption record to the current thread's journal
   * directory. Serializes in-flight tool calls and active subagents for crash
   * recovery.
   */
  void writeInterruptionRecord();

  /**
   * @brief Gets the history for a specific agent.
   */
  shared::AgentHistory getAgentHistory(const std::string &agentId) const;

  /**
   * @brief Gets a shared history pointer for a specific agent.
   * This always returns a detached snapshot loaded from the thread journal.
   * It never exposes the live in-memory history owned by an active agent.
   */
  std::shared_ptr<shared::AgentHistory>
  getAgentHistoryPtr(const std::string &agentId) const;

  bool materializeThreadLeadAgent(const std::string &threadId,
                                  std::string &agentIdOut);

private:
  Harness();
  ~Harness();

  void joinBackgroundThreads();

  // Allow Agent, Engine, and lock tools to use internal messaging
  friend class Agent;
  friend class Engine;
  friend class FleetLockTool;
  friend class FleetLockRespondTool;

  /**
   * Check if an agent is a descendant of another agent.
   * @param agentId The agent ID to check
   * @param ancestorId The potential ancestor ID
   * @param depth Recursion depth for cycle detection
   * @return true if agentId is a descendant of ancestorId
   */
  bool isDescendant(const std::string &agentId, const std::string &ancestorId,
                    int depth = 0);

  /**
   * Emit a HarnessEvent to all subscribers.
   * @param event The event to emit
   */
  void emitEvent(const firmius::shared::AppEvent &event);

  /**
   * Route an EngineEvent to AppEvent and emit if relevant.
   * @param event The AppEvent to route
   */
  void routeEngineEvent(const firmius::shared::AppEvent &event);

  void maybeGenerateTitle(const std::string &threadId,
                          const std::string &firstMessage);
  bool dispatchRequestToAgent(
      const std::string &threadId, const std::string &preferredAgentId,
      const std::string &text,
      const std::vector<firmius::shared::ImageContent> &images,
      std::string &statusMessage);
  std::optional<shared::ThreadMetadata::RetryableRequest>
  snapshotResumableTurnForAgent(const std::string &threadId,
                                const std::string &agentId);
  std::optional<shared::ThreadMetadata::RetryableRequest>
  recoverLastResumableTurnForThread(
      const std::string &threadId,
      const std::string &preferredAgentId = "");
  std::optional<std::string> resolveRetryTargetAgentId(
      const std::string &threadId,
      const std::string &preferredAgentId = "");
  std::optional<std::string>
  materializeLeadAgentIdentity(const std::string &threadId);

  /**
   * Drains queued messages targeting the specified agent when that agent is
   * ready to accept new input.
   */
  void drainQueueForAgent(const std::string &agentId,
                         const std::string &threadId,
                         bool allowRunningInjection = false);
  void drainInternalQueueForAgent(const std::string &agentId,
                                  const std::string &threadId);
  void drainInternalQueueForAgent(const std::string &agentId,
                                  const std::string &threadId,
                                  bool allowRunningInjection);
  void queueInternalMessage(const std::string &agentId,
                            const std::string &threadId,
                            const std::string &text);
  void appendInternalMessage(std::shared_ptr<shared::IAgent> agent,
                             const std::string &text);
  std::string resolveFleetRoot(const std::string &agentId);
  std::vector<std::string> listFleetPeers(const std::string &agentId,
                                          const std::string &threadId);
  std::size_t failOwnedLocks(const std::string &agentId,
                             const std::string &threadId,
                             const std::string &reason);

  void clearQueue();
  void clearQueueForAgentThread(const std::string &agentId,
                                const std::string &threadId);

  // Message queue for sending while agent is running
  // Message queue entry: messageId, text, images
  struct QueuedMessage {
    std::string id;
    std::string text;
    std::vector<firmius::shared::ImageContent> images;
    std::string threadId;
    std::string agentId;
  };
  std::deque<QueuedMessage> messageQueue_;

  struct QueuedInternalMessage {
    std::string id;
    std::string text;
    std::string threadId;
    std::string agentId;
  };
  std::deque<QueuedInternalMessage> internalQueue_;

  // Background model caching
  std::vector<shared::ModelInfo> cachedModels_;
  mutable std::mutex modelsMutex_;
  std::unordered_set<std::string> cachedModelKeys_;
  std::unordered_set<std::string> loadingModelProviders_;
  bool isRefreshingModels_ = false;
  bool modelsLoaded_ = false;
  bool engineListenerRegistered_ = false;

  std::string currentThreadId_;
  std::string focusedAgentId_;
  ThreadManager threadManager_;
  ThreadLockManager lockManager_;
  std::recursive_mutex mutex_;

  // Subscribers
  firmius::shared::utils::FastHash<int, std::function<void(const firmius::shared::AppEvent &)>> subscribers_;
  int nextSubscriptionId_ = 0;

  // Agent state tracking for event routing
  firmius::shared::utils::FastHash<std::string, std::string> threadAgentMap_; // threadId -> focusedAgentId

  // Track which threads have had titles generated
  std::unordered_set<std::string> titleGeneratedThreads_;

  struct PendingPermissionRequest {
    std::mutex mutex;
    std::condition_variable cv;
    bool resolved = false;
    PermissionResponse response = PermissionResponse::Deny;
    PermissionEscalationRequest request;
  };
  firmius::shared::utils::FastHash<std::string, std::shared_ptr<PendingPermissionRequest>> pendingPermissionRequests_;
  uint64_t nextPermissionRequestId_ = 0;

  // Tracking for detached background tasks (e.g. title generation)
  std::vector<std::thread> backgroundThreads_;
};

} // namespace firmius::core
