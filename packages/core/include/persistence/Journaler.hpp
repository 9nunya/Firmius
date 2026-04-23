#ifndef FIRMIUS_CORE_JOURNALER_HPP
#define FIRMIUS_CORE_JOURNALER_HPP

#include "Context.hpp"
#include <string>
#include <mutex>
#include <queue>
#include <memory>
#include <thread>
#include <condition_variable>
#include <variant>

struct sqlite3;

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
    sqlite3* openWithRetry();
    void writeTurn(sqlite3*& db, const AgentTurn& turn);
    void performRewrite(sqlite3*& db, const std::vector<AgentTurn>& turns);

    std::string filePath;
    std::string threadId_;
    std::string agentId_;
    std::shared_ptr<std::mutex> dbMutex;
    
    std::queue<JournalOp> queue;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::thread workerThread;
    bool stopWorker = false;
};

}

#endif
