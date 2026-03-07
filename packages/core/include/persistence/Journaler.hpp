#ifndef FIRMIUS_CORE_JOURNALER_HPP
#define FIRMIUS_CORE_JOURNALER_HPP

#include "Context.hpp"
#include <string>
#include <fstream>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <variant>

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

    /**
     * @brief Rewrites the entire journal with the provided turns.
     * @param turns The complete set of turns to write.
     */
    void rewriteJournal(const std::vector<AgentTurn>& turns);

    /**
     * @brief Gets the file path for this journal.
     * @return The file path.
     */
    const std::string& getFilePath() const { return filePath; }

private:
    struct AppendOp {
        AgentTurn turn;
    };

    struct RewriteOp {
        std::vector<AgentTurn> turns;
    };

    using JournalOp = std::variant<AppendOp, RewriteOp>;

    void processQueue();
    void writeTurn(const AgentTurn& turn);
    void performRewrite(const std::vector<AgentTurn>& turns);

    std::string filePath;
    std::ofstream file;
    
    std::queue<JournalOp> queue;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::jthread workerThread;
    bool stopWorker = false;
};

}

#endif
