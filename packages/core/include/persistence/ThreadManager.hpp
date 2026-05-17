#ifndef FIRMIUS_CORE_THREAD_MANAGER_HPP
#define FIRMIUS_CORE_THREAD_MANAGER_HPP

#include "Context.hpp"
#include "ICommandIntent.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <map>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Entry in the agent manifest for a thread.
 */
struct AgentManifestEntry {
    std::string persona;
    std::string parentId;
    std::string friendlyName;
    std::string title;
    bool persistHistory;
};

struct CommandAllowRule {
    std::string exactCommand;
    std::string normalizedCommand;
    std::string primaryCommand;
    CommandSeverity severity = CommandSeverity::LOW;
    std::string toolName;
    bool appliesToEntireTool = false;
    bool isGlobal = false;

    CommandAllowRule() = default;
    CommandAllowRule(std::string exact, std::string normalized, std::string primary,
                     CommandSeverity sev)
        : exactCommand(std::move(exact)), normalizedCommand(std::move(normalized)), primaryCommand(std::move(primary)), severity(sev) {}

    bool operator==(const CommandAllowRule& other) const = default;
};

struct PathAllowRule {
    std::string pathPrefix;
    std::string toolName;
    bool readOnly = false;
    bool appliesToAllReads = false;
    bool isGlobal = false;

    bool operator==(const PathAllowRule& other) const = default;
};

struct ThreadPermissionRules {
    std::vector<CommandAllowRule> commandAllowRules;
    std::vector<PathAllowRule> pathAllowRules;
    std::vector<std::string> writeAllowPaths;
    bool allowAllReadsSession = false;
    std::vector<std::string> allowAllToolSessions;

    bool operator==(const ThreadPermissionRules& other) const = default;
};



struct AgentLiveState {
    std::string threadId;
    std::string agentId;

    bool operator==(const AgentLiveState& other) const = default;
};

struct FleetLock {
    std::string lockId;
    std::string threadId;
    std::string rootAgentId;
    std::string ownerAgentId;
    std::string status;
    std::string reason;
    std::vector<std::string> paths;
    std::vector<std::string> waiters;
    uint64_t createdAt = 0;
    uint64_t updatedAt = 0;

    bool operator==(const FleetLock& other) const = default;
};

struct FleetState {
    std::vector<FleetLock> locks;

    bool operator==(const FleetState& other) const = default;
};

struct CompactionSnapshot {
    std::string compactionId;
    std::string threadId;
    std::string agentId;
    uint32_t previousContextSize = 0;
    uint64_t createdAt = 0;
    std::vector<AgentTurn> turns;

    bool operator==(const CompactionSnapshot& other) const = default;
};



/**
 * @brief Manages thread directory structure and metadata.
 */
class ThreadManager {
public:
    static std::string defaultBasePath();
    static std::string threadDirectoryPath(const std::string& basePath,
                                           const std::string& threadId);
    static std::string compactionSnapshotPath(const std::string& basePath,
                                              const std::string& threadId,
                                              const std::string& agentId);

    /**
     * @brief Constructs a ThreadManager rooted at the given base path.
     * @param basePath Directory where thread subdirectories are stored.
     */
    explicit ThreadManager(std::string basePath);

    /**
     * @brief Lists all available thread IDs.
     */
    std::vector<std::string> listThreads() const;

    /**
     * @brief Creates a new thread and saves its metadata.
     * @param metadata Thread metadata.
     * @return The generated thread ID.
     */
    std::string createThread(const ThreadMetadata& metadata);

    /**
     * @brief Loads metadata for a thread.
     */
    ThreadMetadata getMetadata(const std::string& threadId) const;
    bool tryGetMetadata(const std::string& threadId, ThreadMetadata& metadata,
                        std::string* error = nullptr) const;

    /**
     * @brief Loads the full history for a specific agent in a thread.
     */
    AgentHistory loadAgentHistory(const std::string& threadId, const std::string& agentId) const;

    /**
     * @brief Discovers all agent IDs associated with a thread.
     */
    std::vector<std::string> listAgents(const std::string& threadId) const;

    void updateHostIdentifier(const std::string& threadId, const std::string& hostIdentifier);
    void deleteThread(const std::string& threadId);
    void updateMetadata(const std::string& threadId, const ThreadMetadata& metadata);
    std::vector<ThreadMetadata> listThreadsWithMetadata() const;
    AgentTodoList getAgentTodo(const std::string& threadId,
                               const std::string& agentId) const;
    void writeAgentTodo(const std::string& threadId, const std::string& agentId,
                        const AgentTodoList& todoList);
    AgentTodoList mutateAgentTodo(
        const std::string& threadId, const std::string& agentId,
        const std::function<void(AgentTodoList&)>& mutator);
    AgentLiveState getAgentLiveState(const std::string& threadId,
                                     const std::string& agentId) const;
    void writeAgentLiveState(const std::string& threadId,
                             const std::string& agentId,
                             const AgentLiveState& liveState);
    AgentLiveState mutateAgentLiveState(
        const std::string& threadId, const std::string& agentId,
        const std::function<void(AgentLiveState&)>& mutator);
    FleetState getFleetState(const std::string& threadId) const;
    void writeFleetState(const std::string& threadId, const FleetState& state);
    FleetState mutateFleetState(
        const std::string& threadId,
        const std::function<void(FleetState&)>& mutator);
    std::vector<CompactionSnapshot>
    loadCompactionSnapshots(const std::string& threadId,
                            const std::string& agentId) const;
    void appendCompactionSnapshot(const std::string& threadId,
                                  const std::string& agentId,
                                  const CompactionSnapshot& snapshot);
    bool popCompactionSnapshot(const std::string& threadId,
                               const std::string& agentId,
                               const std::optional<std::string>& compactionId =
                                   std::nullopt,
                               CompactionSnapshot* removed = nullptr);
    void writeEditBatch(const std::string& threadId,
                        const shared::EditBatchSummary& summary,
                        const std::vector<shared::EditFileMutation>& files);
    shared::EditBatchDetail getEditBatch(const std::string& threadId,
                                         const std::string& editBatchId) const;
    std::vector<shared::EditBatchSummary>
    listEditBatches(const std::string& threadId,
                    const shared::EditHistoryFilters& filters = {}) const;
    std::vector<shared::EditFileMutation>
    listEditFileMutationsForFile(const std::string& threadId,
                                 const std::string& filePath) const;
    void updateEditBatchStatus(
        const std::string& threadId, const std::string& editBatchId,
        shared::EditBatchStatus status,
        const std::optional<std::string>& undoActionBatchId = std::nullopt);
    void updateEditFileMutationStatus(
        const std::string& threadId, const std::string& fileMutationId,
        shared::EditFileMutationStatus status);
    void writeEditUndoAction(const std::string& threadId,
                             const shared::EditUndoAction& action);
    std::optional<shared::EditUndoAction>
    findEditUndoAction(const std::string& threadId,
                       const std::string& undoActionId) const;
    void writeEditRedoAction(const std::string& threadId,
                             const shared::EditRedoAction& action);
    std::optional<shared::EditRedoAction>
    findEditRedoAction(const std::string& threadId, const std::string& redoActionId) const;
    void writeTranscriptUndoAction(const std::string& threadId,
                                   const shared::TranscriptUndoAction& action,
                                   const std::vector<shared::TranscriptRedoPayload>& payloads);
    std::optional<shared::TranscriptUndoAction>
    findTranscriptUndoAction(const std::string& threadId,
                             const std::string& undoActionId) const;
    std::vector<shared::TranscriptUndoAction>
    listTranscriptUndoActions(const std::string& threadId, int limit = 25) const;
    void markTranscriptUndoRedoAvailability(const std::string& threadId,
                                            const std::string& undoActionId, bool available);
    std::vector<shared::TranscriptRedoPayload> loadTranscriptRedoPayloads(
        const std::string& threadId, const std::string& undoActionId) const;

    /**
     * @brief Reads the agent manifest for a thread.
     * @return Map of agentId to manifest entry.
     */
    std::map<std::string, AgentManifestEntry> readAgentManifest(const std::string& threadId) const;
    bool tryReadAgentManifest(const std::string& threadId,
                              std::map<std::string, AgentManifestEntry>& manifest,
                              std::string* error = nullptr) const;

    void addWriteAllowPath(const std::string& threadId, const std::string& pathPrefix);
    /**
     * @brief Writes the agent manifest for a thread.
     */
    void writeAgentManifest(const std::string& threadId, const std::map<std::string, AgentManifestEntry>& manifest);

    ThreadPermissionRules readPermissionRules(const std::string& threadId) const;
    void writePermissionRules(const std::string& threadId, const ThreadPermissionRules& rules);
    void addCommandAllowRule(const std::string& threadId, const CommandAllowRule& rule);
    void addPathAllowRule(const std::string& threadId, const PathAllowRule& rule);
    void setAllowAllReadsSession(const std::string& threadId, bool enabled);
    void addAllowAllToolSession(const std::string& threadId, const std::string& toolName);

    shared::ThreadArtifactMetadata writeArtifact(
        const std::string& threadId, const std::string& ownerAgentId,
        const std::string& ownerFriendlyName, const std::string& filename,
        const std::string& content, bool* created = nullptr,
        const std::optional<std::string>& kind = std::nullopt,
        const std::optional<std::string>& description = std::nullopt);
    std::string readArtifact(const std::string& threadId,
                             const std::string& ownerAgentId,
                             const std::string& filename) const;
    std::vector<shared::ThreadArtifactMetadata>
    listArtifacts(const std::string& threadId) const;
    std::vector<shared::ThreadArtifactMetadata>
    listArtifactsForAgent(const std::string& threadId,
                          const std::string& ownerAgentId) const;
    std::optional<std::string>
    findAgentIdByFriendlyName(const std::string& threadId,
                              const std::string& friendlyName) const;
    std::optional<std::string>
    findFriendlyNameByAgentId(const std::string& threadId,
                              const std::string& agentId) const;

private:
    std::string basePath_;
};

}

#endif
