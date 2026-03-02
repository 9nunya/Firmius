#ifndef FIRMIUS_CORE_JOURNALER_HPP
#define FIRMIUS_CORE_JOURNALER_HPP

#include "Context.hpp"
#include <string>
#include <fstream>
#include <mutex>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Thread-safe, append-only logger for agent turns.
 */
class Journaler {
public:
    /**
     * @brief Constructs a Journaler for a specific agent in a thread.
     * @param threadId The thread ID.
     * @param agentId The agent ID.
     */
    Journaler(const std::string& threadId, const std::string& agentId);
    ~Journaler();

    /**
     * @brief Appends a full turn to the journal and flushes to disk.
     * @param turn The turn to persist.
     */
    void appendTurn(const AgentTurn& turn);

private:
    std::string filePath;
    std::ofstream file;
    std::mutex mutex;
};

}

#endif
