#ifndef FIRMIUS_CORE_THREAD_MANAGER_HPP
#define FIRMIUS_CORE_THREAD_MANAGER_HPP

#include "Context.hpp"
#include <string>
#include <vector>
#include <map>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Manages thread directory structure and metadata.
 */
class ThreadManager {
public:
    /**
     * @brief Lists all available thread IDs.
     */
    static std::vector<std::string> listThreads();

    /**
     * @brief Creates a new thread and saves its metadata.
     * @param metadata Thread metadata.
     * @return The generated thread ID.
     */
    static std::string createThread(const ThreadMetadata& metadata);

    /**
     * @brief Loads metadata for a thread.
     */
    static ThreadMetadata getMetadata(const std::string& threadId);

    /**
     * @brief Loads the full history for a specific agent in a thread.
     */
    static AgentHistory loadAgentHistory(const std::string& threadId, const std::string& agentId);

    /**
     * @brief Discovers all agent IDs associated with a thread.
     */
    static std::vector<std::string> listAgents(const std::string& threadId);
};

}

#endif
