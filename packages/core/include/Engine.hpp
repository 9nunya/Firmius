#ifndef FIRMIUS_CORE_ENGINE_HPP
#define FIRMIUS_CORE_ENGINE_HPP

#include "IAgent.hpp"
#include "IHost.hpp"
#include "IProvider.hpp"
#include "Events.hpp"
#include "persistence/HistoryEditor.hpp"
#include "tools/ToolRegistry.hpp"
#include "Context.hpp"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <future>
#include <map>
#include <optional>
#include <chrono>
#include <unordered_set>
#include "utils/FastHash.hpp"

namespace firmius::core {

using namespace firmius::shared;

struct SummonAgentOverrides {
    std::optional<std::string> cwdOverride;
    std::optional<std::vector<std::string>> allowedPathsOverride;
};

/**
 * @brief The fleet commander. Manages agent lifecycles and event distribution.
 */
class Engine {
public:
    static Engine& instance() {
        static Engine i;
        return i;
    }

    /**
     * @brief Summons a new agent in its own thread.
     */
    std::string summonAgent(const std::string& threadId, const std::string& personaName,
                            const std::string& task = "", bool persistHistory = true,
                            const std::string& parentId = "", const std::string& friendlyName = "",
                            const std::string& title = "", const std::string& requestedAgentId = "",
                            const std::string& providerId = "", const std::string& modelId = "",
                            const std::string& variantName = "",
                            const std::vector<firmius::shared::ImageContent>& images = {},
                            const std::optional<SummonAgentOverrides>& overrides = std::nullopt);

    /**
     * @brief Resumes an existing agent with pre-loaded history.
     */
    std::string resumeAgent(const std::string& threadId, const std::string& agentId,
                            const std::string& personaName, const std::string& parentId,
                            const std::string& friendlyName, const std::string& title,
                            bool persistHistory);

    std::string createAgent(const std::string& threadId, const std::string& personaName,
                            bool persistHistory = true, const std::string& parentId = "",
                            const std::string& friendlyName = "",
                            const std::string& title = "");

    std::optional<AgentOutcome> waitForAgentOutcome(const std::string& agentId, std::optional<std::chrono::milliseconds> timeout = std::nullopt);
    std::optional<AgentOutcome> peekAgentOutcome(const std::string& agentId, std::optional<std::chrono::milliseconds> timeout = std::nullopt);

    /**
     * @brief Adds a listener for all engine events.
     */
    void addEventListener(std::function<void(const AppEvent&)> listener);

    /**
     * @brief Cancels a running agent.
     */
    void cancelAgent(const std::string& agentId);

    /**
     * @brief Lists all currently active agent IDs.
     */
    std::vector<std::string> listActiveAgents() const;

    /**
     * @brief Terminates an existing agent and cleans up its resources.
     */
    void terminateAgent(const std::string& agentId);

    /**
     * @brief Executes a task on an existing agent (re-tasking).
     * @param agentId The agent ID.
     * @param task The task text.
     * @param images Optional images to include with the task.
     */
    void executeTask(const std::string& agentId, const std::string& task,
                     const std::vector<firmius::shared::ImageContent>& images = {});
    void resumeTask(const std::string& agentId);

    /**
     * @brief Compacts an agent's context without resuming execution.
     */
    void compactAgent(const std::string& agentId);

    /**
     * @brief Switches the provider/model for an agent.
     * @param agentId The agent ID.
     * @param providerId The new provider ID.
     * @param modelId The new model ID.
     */
    void switchAgentModel(const std::string& agentId, const std::string& providerId, const std::string& modelId);

    /**
     * @brief Switches the provider/model/variant for an agent.
     * @param agentId The agent ID.
     * @param providerId The new provider ID.
     * @param modelId The new model ID.
     * @param variantName The model variant name (e.g., "low", "medium", "max").
     */
    void switchAgentModel(const std::string& agentId, const std::string& providerId, const std::string& modelId,
                          const std::string& variantName);

    /**
     * @brief Undoes the last N turns for an agent.
     * @param agentId The agent ID.
     * @param count Number of turns to undo.
     * @return Result of the undo operation.
     */
    UndoResult undoAgentTurns(const std::string& agentId, int count);

    /**
     * @brief Undoes the last N messages for an agent.
     * @param agentId The agent ID.
     * @param count Number of messages to undo.
     * @return Result of the undo operation.
     */
    UndoResult undoAgentMessages(const std::string& agentId, int count);

    /**
     * @brief Undoes all turns after a specific timestamp.
     * @param agentId The agent ID.
     * @param timestamp The timestamp threshold.
     * @return Result of the undo operation.
     */
    UndoResult undoAgentAfterTimestamp(const std::string& agentId, uint64_t timestamp);
    std::vector<shared::EditBatchSummary>
    listAgentEditBatches(const std::string& threadId,
                         const shared::EditHistoryFilters& filters = {}) const;
    shared::EditUndoEligibility evaluateEditBatchUndo(const std::string& threadId,
                                                      const std::string& editBatchId) const;
    /// Eligibility check that knows about a co-undo set: any blocker
    /// whose batch id is in `coUndoBatchIds` is treated as if it were
    /// already undone, because the caller is undoing it in the same
    /// compound. Without this, chains of edits on the same file in one
    /// turn would always block their predecessors.
    shared::EditUndoEligibility evaluateEditBatchUndo(
        const std::string& threadId,
        const std::string& editBatchId,
        const std::unordered_set<std::string>& coUndoBatchIds) const;
    shared::EditRedoEligibility evaluateEditBatchRedo(const std::string& threadId,
                                                      const std::string& undoActionId) const;
    shared::TranscriptUndoAction undoAgentTurnsWithRedo(const std::string& agentId, int count);
    shared::TranscriptUndoAction undoAgentMessagesWithRedo(const std::string& agentId, int count);
    shared::TranscriptUndoAction undoAgentAfterTimestampWithRedo(const std::string& agentId,
                                                                 uint64_t timestamp);
    std::optional<shared::EditRedoAction> redoEditUndoAction(const std::string& agentId,
                                                             const std::string& undoActionId);
    shared::TranscriptRedoEligibility evaluateTranscriptRedo(const std::string& threadId,
                                                             const std::string& undoActionId) const;
    std::optional<shared::TranscriptRedoAction> redoTranscriptUndoAction(
        const std::string& agentId, const std::string& undoActionId);
    shared::EditUndoAction undoEditBatch(const std::string& agentId, const std::string& editBatchId);

    /**
     * @brief Shuts down the engine and waits for all threads to finish.
     */
    void shutdown();

    std::vector<firmius::provider::ToolDefinition>
    getAvailableToolDefinitionsForPermissions(
        const firmius::shared::AgentPermissions &permissions) const;

    const ToolRegistry& getToolRegistry() const { return toolRegistry; }

private:
    Engine();
    void initProviders();
    void reap();
    void broadcast(const AppEvent& event);
    void handleStreamEvent(const std::string& agentId, const std::string& parentId, const firmius::shared::StreamEvent& ev, const std::shared_ptr<std::atomic<bool>>& errorBroadcast);

    ToolRegistry toolRegistry;  // Owned by Engine, outlives agents
    
    std::vector<std::function<void(const AppEvent&)>> listeners;
    std::mutex listenerMutex;
    std::vector<std::jthread> fleet;
    std::mutex fleetMutex;
    
    std::vector<std::jthread> taskThreads_;
    std::mutex taskThreadsMutex_;

    firmius::shared::utils::FastHash<std::string, std::shared_future<AgentOutcome>> agentFutures;
    std::mutex futuresMutex;
    firmius::shared::utils::FastHash<std::string, AgentOutcome> completedOutcomes;  // Store outcomes for multiple waits
};

}

#endif
