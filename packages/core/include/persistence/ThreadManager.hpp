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

/**
 * @brief Manages thread directory structure and metadata.
 */
class ThreadManager {
public:
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
