#include "persistence/Journaler.hpp"

#include "Serialization.hpp"
#include "persistence/ThreadManager.hpp"

#include <rapidjson/document.h>
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

constexpr int kMaxOpenRetries = 3;
constexpr int kOpenRetryBaseMs = 50;

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

std::string jsonString(const rapidjson::Document& d) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    return buffer.GetString();
}

void execPrepared(sqlite3* db, const std::string& sql,
                  const std::function<void(sqlite3_stmt*)>& binder,
                  const std::string& context) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(context);
    }
    binder(stmt);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(context + ": " + sqlite3_errmsg(db));
    }
}

sqlite3_int64 insertAgentTurnV2(sqlite3* db, const std::string& threadId,
                                const std::string& agentId,
                                const AgentTurn& turn) {
    const auto turnJson = toJson(turn);
    execPrepared(
        db,
        "INSERT INTO agent_turns_v2(thread_id, agent_id, turn_id, stop_reason, prompt_tokens, completion_tokens, reasoning_tokens, total_tokens, estimated_cost_usd) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, threadId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, agentId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, turn.turnId.c_str(), -1, SQLITE_TRANSIENT);
            const auto stopReason = turnJson.HasMember("stopReason") && turnJson["stopReason"].IsString()
                                        ? std::string(turnJson["stopReason"].GetString())
                                        : std::string("stop");
            sqlite3_bind_text(stmt, 4, stopReason.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(turn.metrics.tokens.prompt));
            sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(turn.metrics.tokens.completion));
            sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(turn.metrics.tokens.reasoning));
            sqlite3_bind_int64(stmt, 8, static_cast<sqlite3_int64>(turn.metrics.tokens.total));
            sqlite3_bind_double(stmt, 9, turn.metrics.estimatedCostUsd);
        },
        "failed to write normalized agent turn");
    return sqlite3_last_insert_rowid(db);
}

void persistAgentTurnMessagesV2(sqlite3* db, sqlite3_int64 turnRowId,
                                const AgentTurn& turn) {
    int messageOrdinal = 0;
    for (const auto& msg : turn.messages) {
        const auto msgJson = toJson(msg);
        execPrepared(
            db,
            "INSERT INTO turn_messages_v2(turn_row_id, message_id, role, visibility, timestamp, parent_id, ordinal) VALUES(?, ?, ?, ?, ?, ?, ?);",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, turnRowId);
                sqlite3_bind_text(stmt, 2, msg.id.c_str(), -1, SQLITE_TRANSIENT);
                const auto role = msgJson.HasMember("role") && msgJson["role"].IsString()
                                      ? std::string(msgJson["role"].GetString())
                                      : std::string();
                const auto visibility = msgJson.HasMember("visibility") && msgJson["visibility"].IsString()
                                            ? std::string(msgJson["visibility"].GetString())
                                            : std::string();
                sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, visibility.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(msg.timestamp));
                if (msg.parentId.has_value()) {
                    sqlite3_bind_text(stmt, 6, msg.parentId->c_str(), -1, SQLITE_TRANSIENT);
                } else {
                    sqlite3_bind_null(stmt, 6);
                }
                sqlite3_bind_int(stmt, 7, messageOrdinal);
            },
            "failed to write normalized turn message");
        const sqlite3_int64 messageRowId = sqlite3_last_insert_rowid(db);

        int partOrdinal = 0;
        for (const auto& part : msg.content) {
            const auto partJson = toJson(part);
            execPrepared(
                db,
                "INSERT INTO message_parts_v2(message_row_id, ordinal, part_type, payload_json) VALUES(?, ?, ?, ?);",
                [&](sqlite3_stmt* stmt) {
                    sqlite3_bind_int64(stmt, 1, messageRowId);
                    sqlite3_bind_int(stmt, 2, partOrdinal);
                    const auto type = partJson.HasMember("type") && partJson["type"].IsString()
                                          ? std::string(partJson["type"].GetString())
                                          : std::string();
                    const auto payload = jsonString(partJson);
                    sqlite3_bind_text(stmt, 3, type.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 4, payload.c_str(), -1, SQLITE_TRANSIENT);
                },
                "failed to write normalized message part");
            partOrdinal++;
        }
        messageOrdinal++;
    }
}

void deleteAgentTurnsV2(sqlite3* db, const std::string& threadId,
                        const std::string& agentId) {
    execPrepared(db,
                 "DELETE FROM message_parts_v2 WHERE message_row_id IN (SELECT m.id FROM turn_messages_v2 m JOIN agent_turns_v2 t ON t.id=m.turn_row_id WHERE t.thread_id=? AND t.agent_id=?);",
                 [&](sqlite3_stmt* stmt) {
                     sqlite3_bind_text(stmt, 1, threadId.c_str(), -1, SQLITE_TRANSIENT);
                     sqlite3_bind_text(stmt, 2, agentId.c_str(), -1, SQLITE_TRANSIENT);
                 },
                 "failed to delete normalized message parts");
    execPrepared(db,
                 "DELETE FROM turn_messages_v2 WHERE turn_row_id IN (SELECT id FROM agent_turns_v2 WHERE thread_id=? AND agent_id=?);",
                 [&](sqlite3_stmt* stmt) {
                     sqlite3_bind_text(stmt, 1, threadId.c_str(), -1, SQLITE_TRANSIENT);
                     sqlite3_bind_text(stmt, 2, agentId.c_str(), -1, SQLITE_TRANSIENT);
                 },
                 "failed to delete normalized turn messages");
    execPrepared(db,
                 "DELETE FROM agent_turns_v2 WHERE thread_id=? AND agent_id=?;",
                 [&](sqlite3_stmt* stmt) {
                     sqlite3_bind_text(stmt, 1, threadId.c_str(), -1, SQLITE_TRANSIENT);
                     sqlite3_bind_text(stmt, 2, agentId.c_str(), -1, SQLITE_TRANSIENT);
                 },
                 "failed to delete normalized agent turns");
}

} // namespace

sqlite3* Journaler::openWithRetry() {
    std::string lastErr;
    for (int attempt = 1; attempt <= kMaxOpenRetries; ++attempt) {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(dbPath().c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            lastErr = db ? sqlite3_errmsg(db) : "unknown sqlite error";
            if (db) sqlite3_close(db);
            if (attempt < kMaxOpenRetries) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kOpenRetryBaseMs * attempt));
                continue;
            }
            throw std::runtime_error(
                "failed to open sqlite journal db (" +
                std::to_string(kMaxOpenRetries) + " attempts): " + lastErr);
        }

        try {
            sqlite3_exec(db, "PRAGMA journal_mode=WAL;",
                         nullptr, nullptr, nullptr);
            execOrThrow(db, "PRAGMA synchronous=NORMAL;",
                        "journal pragma synchronous failed");
            execOrThrow(db, "PRAGMA busy_timeout=5000;",
                        "journal pragma busy_timeout failed");
            execOrThrow(db,
                        "CREATE TABLE IF NOT EXISTS agent_turns_v2 ("
                        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        " thread_id TEXT NOT NULL,"
                        " agent_id TEXT NOT NULL,"
                        " turn_id TEXT NOT NULL,"
                        " stop_reason TEXT NOT NULL DEFAULT 'stop',"
                        " prompt_tokens INTEGER NOT NULL DEFAULT 0,"
                        " completion_tokens INTEGER NOT NULL DEFAULT 0,"
                        " reasoning_tokens INTEGER NOT NULL DEFAULT 0,"
                        " total_tokens INTEGER NOT NULL DEFAULT 0,"
                        " estimated_cost_usd REAL NOT NULL DEFAULT 0,"
                        " UNIQUE(thread_id, agent_id, turn_id)"
                        ");"
                        "CREATE TABLE IF NOT EXISTS turn_messages_v2 ("
                        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        " turn_row_id INTEGER NOT NULL REFERENCES agent_turns_v2(id) ON DELETE CASCADE,"
                        " message_id TEXT NOT NULL DEFAULT '',"
                        " role TEXT NOT NULL DEFAULT '',"
                        " visibility TEXT NOT NULL DEFAULT '',"
                        " timestamp INTEGER NOT NULL DEFAULT 0,"
                        " parent_id TEXT,"
                        " ordinal INTEGER NOT NULL"
                        ");"
                        "CREATE TABLE IF NOT EXISTS message_parts_v2 ("
                        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        " message_row_id INTEGER NOT NULL REFERENCES turn_messages_v2(id) ON DELETE CASCADE,"
                        " ordinal INTEGER NOT NULL,"
                        " part_type TEXT NOT NULL,"
                        " payload_json TEXT NOT NULL"
                        ");",
                        "sqlite schema init failed");
            return db;
        } catch (const std::exception& ex) {
            lastErr = ex.what();
            sqlite3_close(db);
            if (attempt < kMaxOpenRetries) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kOpenRetryBaseMs * attempt));
                continue;
            }
            throw;
        }
    }
    throw std::runtime_error("failed to open sqlite journal db");
}

Journaler::Journaler(const std::string& threadId, const std::string& agentId) {
    threadId_ = threadId;
    agentId_ = agentId;
    filePath = "sqlite://" + dbPath() + "#" + threadId + "/" + agentId;
    dbMutex = acquireJournalMutex(threadId + ":" + agentId);
    workerThread = std::thread([this]() { processQueue(); });
}

Journaler::~Journaler() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopWorker = true;
    }
    queueCv.notify_all();
    drainCv.notify_all();

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

void Journaler::flush() {
    std::unique_lock<std::mutex> lock(queueMutex);
    drainCv.wait(lock,
                 [this]() { return (queue.empty() && !processing_) || stopWorker; });
}

void Journaler::processQueue() {
    sqlite3* db = nullptr;

    auto ensureDb = [&]() {
        if (db) {
            char* err = nullptr;
            const int rc = sqlite3_exec(db, "SELECT 1;",
                                        nullptr, nullptr, &err);
            if (err) sqlite3_free(err);
            if (rc == SQLITE_OK) return;
            sqlite3_close(db);
            db = nullptr;
        }
        db = openWithRetry();
    };

    while (true) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCv.wait(lock, [this]() { return !queue.empty() || stopWorker; });

        if (stopWorker && queue.empty()) {
            drainCv.notify_all();
            break;
        }

        auto op = std::move(queue.front());
        queue.pop();
        processing_ = true;
        lock.unlock();

        try {
            ensureDb();
            std::visit(
                [this, &db](auto&& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, AppendOp>) {
                        writeTurn(db, arg.turn);
                    } else if constexpr (std::is_same_v<T, RewriteOp>) {
                        performRewrite(db, arg.turns);
                    }
                },
                op);
        } catch (const std::exception& ex) {
            std::cerr << "[Journaler] persistence operation failed: "
                      << ex.what() << std::endl;
            if (db) {
                sqlite3_close(db);
                db = nullptr;
            }
        } catch (...) {
            std::cerr << "[Journaler] persistence operation failed: unknown error"
                      << std::endl;
            if (db) {
                sqlite3_close(db);
                db = nullptr;
            }
        }

        {
            std::lock_guard<std::mutex> doneLock(queueMutex);
            processing_ = false;
        }
        drainCv.notify_all();
    }

    if (db) {
        sqlite3_close(db);
    }
}

void Journaler::writeTurn(sqlite3*& db, const AgentTurn& turn) {
    std::lock_guard<std::mutex> lock(*dbMutex);

    const sqlite3_int64 turnRowId = insertAgentTurnV2(db, threadId_, agentId_, turn);
    persistAgentTurnMessagesV2(db, turnRowId, turn);
}

void Journaler::performRewrite(sqlite3*& db, const std::vector<AgentTurn>& turns) {
    std::lock_guard<std::mutex> lock(*dbMutex);

    execOrThrow(db, "BEGIN IMMEDIATE;", "sqlite begin failed");
    try {
        deleteAgentTurnsV2(db, threadId_, agentId_);
        for (const auto& turn : turns) {
            const sqlite3_int64 turnRowId = insertAgentTurnV2(db, threadId_, agentId_, turn);
            persistAgentTurnMessagesV2(db, turnRowId, turn);
        }
        execOrThrow(db, "COMMIT;", "sqlite commit failed");
    } catch (...) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

} // namespace firmius::core
