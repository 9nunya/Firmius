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

    bool operator==(const CommandAllowRule& other) const = default;
};

struct ThreadPermissionRules {
    std::vector<CommandAllowRule> commandAllowRules;
    std::vector<std::string> writeAllowPaths;

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

struct RollingMemoryAnchorRecord {
    std::string anchorId;
    std::string anchorType;
    std::string canonicalText;
    std::string exactQuote;
    std::string importance;
    std::string volatility;
    std::vector<std::string> retrievalTags;
    std::vector<std::string> sourceTurnIds;

    bool operator==(const RollingMemoryAnchorRecord& other) const = default;
};

struct RollingMemoryBridgeRecord {
    std::string bridgeId;
    std::string targetTaskSignature;
    std::vector<std::string> relevantAnchorIds;
    std::vector<std::string> relevantEpisodeIds;
    std::vector<std::string> relevantReflectionIds;
    std::string rationale;
    std::string executionHint;
    std::uint64_t createdAt = 0;

    bool operator==(const RollingMemoryBridgeRecord& other) const = default;
};

struct RollingMemoryChunk {
    std::string chunkId;
    std::string sourceStartTurnId;
    std::string sourceEndTurnId;
    std::vector<std::string> sourceTurnIds;
    std::string chunkKind = "episode";
    std::string summary;
    std::string currentTask;
    std::string suggestedResponse;
    std::string activeGoal;
    std::vector<std::string> keyActions;
    std::vector<std::string> keyToolResults;
    std::vector<std::string> openLoops;
    std::vector<std::string> filesSurfaces;
    std::vector<std::string> retrievalTags;
    std::vector<std::string> derivedFromChunkIds;
    std::vector<std::string> anchorIds;
    std::uint32_t sourceTokens = 0;
    std::uint32_t summaryTokens = 0;
    std::uint64_t createdAt = 0;
    bool buffered = false;
    bool active = false;
    bool superseded = false;

    bool operator==(const RollingMemoryChunk& other) const = default;
};

struct RollingMemoryState {
    std::string threadId;
    std::string agentId;
    std::string lastObservedTurnId;
    std::string lastReflectedObservationId;
    std::uint32_t lastContextWindow = 0;
    std::uint32_t lastBufferThresholdTokens = 0;
    std::uint32_t lastTargetThresholdTokens = 0;
    std::uint32_t lastEmergencyThresholdTokens = 0;
    std::uint32_t lastRetainedTailTokens = 0;
    std::uint64_t lastUpdatedAt = 0;
    bool observationInFlight = false;
    bool reflectionInFlight = false;
    std::vector<RollingMemoryChunk> observationChunks;
    bool bridgeInFlight = false;
    std::vector<RollingMemoryChunk> reflectionChunks;

    std::vector<RollingMemoryAnchorRecord> anchors;
    std::vector<RollingMemoryBridgeRecord> bridges;
    std::string lastBridgeId;
    bool operator==(const RollingMemoryState& other) const = default;
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
    std::string createPlan(const Plan& plan);
    void writePlan(const std::string& threadId, const Plan& plan);
    Plan getPlan(const std::string& threadId, const std::string& planId) const;
    std::vector<Plan> listPlans(const std::string& threadId) const;
    void updatePlan(const std::string& threadId, const Plan& plan);
    Plan mutatePlan(const std::string& threadId, const std::string& planId,
                    const std::function<void(Plan&)>& mutator);
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
    RollingMemoryState loadRollingMemoryState(const std::string& threadId,
                                              const std::string& agentId) const;
    void writeRollingMemoryState(const std::string& threadId,
                                 const std::string& agentId,
                                 const RollingMemoryState& state);

    /**
     * @brief Reads the agent manifest for a thread.
     * @return Map of agentId to manifest entry.
     */
    std::map<std::string, AgentManifestEntry> readAgentManifest(const std::string& threadId) const;
    bool tryReadAgentManifest(const std::string& threadId,
                              std::map<std::string, AgentManifestEntry>& manifest,
                              std::string* error = nullptr) const;

    /**
     * @brief Writes the agent manifest for a thread.
     */
    void writeAgentManifest(const std::string& threadId, const std::map<std::string, AgentManifestEntry>& manifest);

    ThreadPermissionRules readPermissionRules(const std::string& threadId) const;
    void writePermissionRules(const std::string& threadId, const ThreadPermissionRules& rules);
    void addCommandAllowRule(const std::string& threadId, const CommandAllowRule& rule);
    void addWriteAllowPath(const std::string& threadId, const std::string& pathPrefix);

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
