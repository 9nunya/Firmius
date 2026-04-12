#include "persistence/Journaler.hpp"

#include "Serialization.hpp"
#include "persistence/ThreadManager.hpp"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <variant>

namespace firmius::core {

namespace {

std::string dbPath() {
    return (std::filesystem::path(ThreadManager::defaultBasePath()) /
            "firmius_threads.db")
        .string();
}

std::shared_ptr<std::mutex> acquireJournalMutex(const std::string& key) {
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> registry;

    std::lock_guard<std::mutex> guard(registryMutex);
    if (auto existing = registry[key].lock()) {
        return existing;
    }

    auto created = std::make_shared<std::mutex>();
    registry[key] = created;
    return created;
}

void execOrThrow(sqlite3* db, const std::string& sql, const char* context) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string message = err ? err : "unknown sqlite error";
        if (err) {
            sqlite3_free(err);
        }
        throw std::runtime_error(std::string(context) + ": " + message);
    }
}

std::string turnToJson(const AgentTurn& turn) {
    rapidjson::Document d = toJson(turn);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    return buffer.GetString();
}

sqlite3* openJournalDb() {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath().c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return nullptr;
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
    execOrThrow(db,
                "CREATE TABLE IF NOT EXISTS agent_turns ("
                " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                " thread_id TEXT NOT NULL,"
                " agent_id TEXT NOT NULL,"
                " turn_json TEXT NOT NULL"
                ");",
                "sqlite schema init failed");
    return db;
}

} // namespace

Journaler::Journaler(const std::string& threadId, const std::string& agentId) {
    threadId_ = threadId;
    agentId_ = agentId;
    filePath = "sqlite://" + dbPath() + "#" + threadId + "/" + agentId;
    dbMutex = acquireJournalMutex(threadId + ":" + agentId);
    workerThread = std::jthread([this](std::stop_token) { processQueue(); });
}

Journaler::~Journaler() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopWorker = true;
    }
    queueCv.notify_all();

    if (workerThread.joinable()) {
        workerThread.join();
    }
}

void Journaler::appendTurn(const AgentTurn& turn) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push(AppendOp{turn});
    }
    queueCv.notify_one();
}

void Journaler::rewriteJournal(const std::vector<AgentTurn>& turns) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push(RewriteOp{turns});
    }
    queueCv.notify_one();
}

void Journaler::processQueue() {
    while (true) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCv.wait(lock, [this]() { return !queue.empty() || stopWorker; });

        if (stopWorker && queue.empty()) {
            break;
        }

        auto op = std::move(queue.front());
        queue.pop();
        lock.unlock();

        try {
            std::visit(
                [this](auto&& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, AppendOp>) {
                        writeTurn(arg.turn);
                    } else if constexpr (std::is_same_v<T, RewriteOp>) {
                        performRewrite(arg.turns);
                    }
                },
                op);
        } catch (const std::exception& ex) {
            std::cerr << "[Journaler] persistence operation failed: "
                      << ex.what() << std::endl;
        } catch (...) {
            std::cerr << "[Journaler] persistence operation failed: unknown error"
                      << std::endl;
        }
    }
}

void Journaler::writeTurn(const AgentTurn& turn) {
    std::lock_guard<std::mutex> lock(*dbMutex);
    sqlite3* db = openJournalDb();
    if (!db) {
        throw std::runtime_error("failed to open sqlite journal db");
    }

    const std::string json = turnToJson(turn);
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        sqlite3_stmt* insertStmt = nullptr;
        if (sqlite3_prepare_v2(db,
                               "INSERT INTO agent_turns(thread_id, agent_id, turn_json) VALUES(?, ?, ?);",
                               -1, &insertStmt, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            throw std::runtime_error("failed to prepare journal insert");
        }
        sqlite3_bind_text(insertStmt, 1, threadId_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insertStmt, 2, agentId_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insertStmt, 3, json.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(insertStmt);
        sqlite3_finalize(insertStmt);
        if (rc == SQLITE_DONE) {
            sqlite3_close(db);
            return;
        }
        if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && attempt < kMaxAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20 * attempt));
            continue;
        }
        const std::string err = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("failed to insert journal turn: " + err);
    }
    sqlite3_close(db);
}

void Journaler::performRewrite(const std::vector<AgentTurn>& turns) {
    std::lock_guard<std::mutex> lock(*dbMutex);
    sqlite3* db = openJournalDb();
    if (!db) {
        throw std::runtime_error("failed to open sqlite journal db");
    }

    execOrThrow(db, "BEGIN IMMEDIATE;", "sqlite begin failed");

    sqlite3_stmt* deleteStmt = nullptr;
    if (sqlite3_prepare_v2(
        db, "DELETE FROM agent_turns WHERE thread_id=? AND agent_id=?;", -1,
        &deleteStmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        throw std::runtime_error("failed to prepare journal rewrite delete");
    }
    sqlite3_bind_text(deleteStmt, 1, threadId_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(deleteStmt, 2, agentId_.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(deleteStmt) != SQLITE_DONE) {
        sqlite3_finalize(deleteStmt);
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        const std::string err = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("failed to delete journal turns: " + err);
    }
    sqlite3_finalize(deleteStmt);

    sqlite3_stmt* insertStmt = nullptr;
    if (sqlite3_prepare_v2(
        db, "INSERT INTO agent_turns(thread_id, agent_id, turn_json) VALUES(?, ?, ?);",
        -1, &insertStmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        throw std::runtime_error("failed to prepare journal rewrite insert");
    }
    for (const auto& turn : turns) {
        const std::string json = turnToJson(turn);
        sqlite3_bind_text(insertStmt, 1, threadId_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insertStmt, 2, agentId_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insertStmt, 3, json.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insertStmt) != SQLITE_DONE) {
            sqlite3_finalize(insertStmt);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            const std::string err = sqlite3_errmsg(db);
            sqlite3_close(db);
            throw std::runtime_error("failed to rewrite journal turns: " + err);
        }
        sqlite3_reset(insertStmt);
        sqlite3_clear_bindings(insertStmt);
    }
    sqlite3_finalize(insertStmt);

    execOrThrow(db, "COMMIT;", "sqlite commit failed");
    sqlite3_close(db);
}

} // namespace firmius::core
