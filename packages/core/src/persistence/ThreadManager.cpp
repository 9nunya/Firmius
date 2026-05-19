#include "persistence/ThreadManager.hpp"

#include "AgentRegistry.hpp"
#include "Serialization.hpp"
#include "utils/JsonUtil.hpp"
#include "utils/PlatformPaths.hpp"
#include "utils/StringUtil.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace firmius::core {

using namespace firmius::shared;

namespace {

static constexpr int kSqliteBusyTimeoutMs = 5000;

using shared::StringUtil;

bool ensureWritableDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec || !std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        return false;
    }

    const auto probe = dir / (".write_probe_" + StringUtil::generateUuid());
    std::ofstream out(probe);
    if (!out.is_open()) {
        return false;
    }
    out << "ok";
    out.close();
    std::filesystem::remove(probe, ec);
    return true;
}

uint64_t nowEpochMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string dbPathForBase(const std::string& basePath) {
    return (std::filesystem::path(basePath) / "firmius_threads.db").string();
}

rapidjson::Document parseJson(const std::string& text,
                              const std::string& context) {
    rapidjson::Document d;
    d.Parse(text.c_str());
    if (d.HasParseError()) {
        throw std::runtime_error("Invalid JSON in " + context);
    }
    return d;
}

void throwSqliteError(sqlite3* db, const std::string& context) {
    throw std::runtime_error(context + ": " +
                             std::string(sqlite3_errmsg(db)));
}

void execSql(sqlite3* db, const std::string& sql,
             const std::string& context) {
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        const std::string message = errMsg ? errMsg : "unknown sqlite error";
        if (errMsg) {
            sqlite3_free(errMsg);
        }
        throw std::runtime_error(context + ": " + message);
    }
}

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql,
              const std::string& context)
        : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            throwSqliteError(db_, context);
        }
    }

    ~Statement() {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
    }

    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    if (sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) !=
        SQLITE_OK) {
        throw std::runtime_error("Failed to bind sqlite text parameter");
    }
}

void bindOptionalText(sqlite3_stmt* stmt, int index,
                      const std::optional<std::string>& value) {
    if (value.has_value()) {
        bindText(stmt, index, *value);
        return;
    }
    if (sqlite3_bind_null(stmt, index) != SQLITE_OK) {
        throw std::runtime_error("Failed to bind sqlite null parameter");
    }
}

template <typename Fn>
auto withImmediateTransaction(sqlite3* db, Fn&& fn) {
    execSql(db, "BEGIN IMMEDIATE;", "Failed to begin sqlite transaction");
    try {
        if constexpr (std::is_void_v<decltype(fn())>) {
            fn();
            execSql(db, "COMMIT;", "Failed to commit sqlite transaction");
            return;
        } else {
            auto result = fn();
            execSql(db, "COMMIT;", "Failed to commit sqlite transaction");
            return result;
        }
    } catch (...) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

constexpr int kConnMaxRetries = 3;
constexpr int kConnRetryBaseMs = 50;
constexpr int kCurrentSchemaVersion = 2;

const char* kLegacySchemaSQL =
    "CREATE TABLE IF NOT EXISTS threads ("
    " thread_id TEXT PRIMARY KEY,"
    " metadata_json TEXT NOT NULL,"
    " created_at INTEGER NOT NULL,"
    " last_active_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS thread_states ("
    " thread_id TEXT PRIMARY KEY,"
    " agent_manifest_json TEXT,"
    " permission_rules_json TEXT,"
    " fleet_state_json TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS agent_turns ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " thread_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL,"
    " turn_json TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_agent_turns_thread_agent"
    " ON agent_turns(thread_id, agent_id, id);"
    "CREATE TABLE IF NOT EXISTS agent_todos ("
    " thread_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL,"
    " todo_json TEXT NOT NULL,"
    " PRIMARY KEY(thread_id, agent_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS agent_live_state ("
    " thread_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL,"
    " state_json TEXT NOT NULL,"
    " PRIMARY KEY(thread_id, agent_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS compaction_snapshots ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " thread_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL,"
    " compaction_id TEXT NOT NULL,"
    " snapshot_json TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_compaction_thread_agent"
    " ON compaction_snapshots(thread_id, agent_id, id);"
    "CREATE TABLE IF NOT EXISTS artifacts ("
    " thread_id TEXT NOT NULL,"
    " owner_agent_id TEXT NOT NULL,"
    " owner_friendly_name TEXT NOT NULL,"
    " filename TEXT NOT NULL,"
    " storage_path TEXT NOT NULL,"
    " content TEXT NOT NULL,"
    " kind TEXT,"
    " description TEXT,"
    " created_at INTEGER NOT NULL,"
    " updated_at INTEGER NOT NULL,"
    " PRIMARY KEY(thread_id, owner_agent_id, filename)"
    ");";

const char* kNormalizedSchemaSQL =
    "CREATE TABLE IF NOT EXISTS schema_meta ("
    " key TEXT PRIMARY KEY,"
    " value TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS thread_metadata_v2 ("
    " thread_id TEXT PRIMARY KEY,"
    " title TEXT NOT NULL DEFAULT '',"
    " host_type TEXT NOT NULL DEFAULT '',"
    " host_identifier TEXT NOT NULL DEFAULT '',"
    " cwd TEXT NOT NULL DEFAULT '',"
    " lead_persona TEXT NOT NULL DEFAULT '',"
    " is_benchmark_run INTEGER NOT NULL DEFAULT 0,"
    " benchmark_id TEXT NOT NULL DEFAULT '',"
    " benchmark_task_id TEXT NOT NULL DEFAULT '',"
    " active_plan_id TEXT NOT NULL DEFAULT '',"
    " permission_mode TEXT NOT NULL DEFAULT 'Request',"
    " created_at INTEGER NOT NULL DEFAULT 0,"
    " last_active_at INTEGER NOT NULL DEFAULT 0,"
    " host_options_json TEXT NOT NULL DEFAULT '{}',"
    " retryable_request_json TEXT"
    ");"
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
    "CREATE INDEX IF NOT EXISTS idx_agent_turns_v2_thread_agent"
    " ON agent_turns_v2(thread_id, agent_id, id);"
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
    "CREATE INDEX IF NOT EXISTS idx_turn_messages_v2_turn"
    " ON turn_messages_v2(turn_row_id, ordinal);"
    "CREATE TABLE IF NOT EXISTS message_parts_v2 ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " message_row_id INTEGER NOT NULL REFERENCES turn_messages_v2(id) ON DELETE CASCADE,"
    " ordinal INTEGER NOT NULL,"
    " part_type TEXT NOT NULL,"
    " payload_json TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_message_parts_v2_message"
    " ON message_parts_v2(message_row_id, ordinal);"
    "CREATE TABLE IF NOT EXISTS agent_todos_v2 ("
    " thread_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL,"
    " next_id INTEGER NOT NULL DEFAULT 1,"
    " PRIMARY KEY(thread_id, agent_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS todo_items_v2 ("
    " thread_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL,"
    " item_id INTEGER NOT NULL,"
    " text TEXT NOT NULL DEFAULT '',"
    " status TEXT NOT NULL DEFAULT 'pending',"
    " chunk_id TEXT NOT NULL DEFAULT '',"
    " plan_id TEXT NOT NULL DEFAULT '',"
    " created_at INTEGER NOT NULL DEFAULT 0,"
    " updated_at INTEGER NOT NULL DEFAULT 0,"
    " PRIMARY KEY(thread_id, agent_id, item_id),"
    " FOREIGN KEY(thread_id, agent_id) REFERENCES agent_todos_v2(thread_id, agent_id) ON DELETE CASCADE"
    ");"
    "CREATE TABLE IF NOT EXISTS agent_live_state_v2 ("
    " thread_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL,"
    " PRIMARY KEY(thread_id, agent_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS fleet_locks_v2 ("
    " thread_id TEXT NOT NULL,"
    " lock_id TEXT NOT NULL,"
    " root_agent_id TEXT NOT NULL DEFAULT '',"
    " owner_agent_id TEXT NOT NULL DEFAULT '',"
    " status TEXT NOT NULL DEFAULT '',"
    " reason TEXT NOT NULL DEFAULT '',"
    " created_at INTEGER NOT NULL DEFAULT 0,"
    " updated_at INTEGER NOT NULL DEFAULT 0,"
    " PRIMARY KEY(thread_id, lock_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS fleet_lock_lists_v2 ("
    " thread_id TEXT NOT NULL,"
    " lock_id TEXT NOT NULL,"
    " list_kind TEXT NOT NULL,"
    " value TEXT NOT NULL,"
    " ordinal INTEGER NOT NULL DEFAULT 0,"
    " PRIMARY KEY(thread_id, lock_id, list_kind, ordinal),"
    " FOREIGN KEY(thread_id, lock_id) REFERENCES fleet_locks_v2(thread_id, lock_id) ON DELETE CASCADE"
    ");"
    "CREATE TABLE IF NOT EXISTS compaction_snapshots_v2 ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " thread_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL,"
    " compaction_id TEXT NOT NULL,"
    " previous_context_size INTEGER NOT NULL DEFAULT 0,"
    " created_at INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_compaction_snapshots_v2_thread_agent"
    " ON compaction_snapshots_v2(thread_id, agent_id, id);"
    "CREATE TABLE IF NOT EXISTS compaction_snapshot_turns_v2 ("
    " snapshot_row_id INTEGER NOT NULL REFERENCES compaction_snapshots_v2(id) ON DELETE CASCADE,"
    " ordinal INTEGER NOT NULL,"
    " turn_json TEXT NOT NULL,"
    " PRIMARY KEY(snapshot_row_id, ordinal)"
    ");"
    "CREATE TABLE IF NOT EXISTS agent_manifest_v2 ("
    " thread_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL,"
    " persona TEXT NOT NULL DEFAULT '',"
    " parent_id TEXT NOT NULL DEFAULT '',"
    " friendly_name TEXT NOT NULL DEFAULT '',"
    " title TEXT NOT NULL DEFAULT '',"
    " persist_history INTEGER NOT NULL DEFAULT 1,"
    " PRIMARY KEY(thread_id, agent_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS artifacts_v2 ("
    " thread_id TEXT NOT NULL,"
    " owner_agent_id TEXT NOT NULL,"
    " filename TEXT NOT NULL,"
    " owner_friendly_name TEXT NOT NULL DEFAULT '',"
    " storage_path TEXT NOT NULL DEFAULT '',"
    " content TEXT NOT NULL DEFAULT '',"
    " kind TEXT,"
    " description TEXT,"
    " created_at INTEGER NOT NULL DEFAULT 0,"
    " updated_at INTEGER NOT NULL DEFAULT 0,"
    " PRIMARY KEY(thread_id, owner_agent_id, filename)"
    ");"
    "CREATE TABLE IF NOT EXISTS edit_batches_v1 ("
    " thread_id TEXT NOT NULL,"
    " edit_batch_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL DEFAULT '',"
    " parent_agent_id TEXT NOT NULL DEFAULT '',"
    " friendly_name TEXT NOT NULL DEFAULT '',"
    " turn_id TEXT NOT NULL DEFAULT '',"
    " tool_call_id TEXT NOT NULL DEFAULT '',"
    " tool_name TEXT NOT NULL DEFAULT '',"
    " request_mode TEXT NOT NULL DEFAULT '',"
    " created_at INTEGER NOT NULL DEFAULT 0,"
    " status TEXT NOT NULL DEFAULT 'Applied',"
    " summary_json TEXT NOT NULL DEFAULT '{}',"
    " undo_action_batch_id TEXT,"
    " PRIMARY KEY(thread_id, edit_batch_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_edit_batches_v1_thread_created"
    " ON edit_batches_v1(thread_id, created_at DESC);"
    "CREATE INDEX IF NOT EXISTS idx_edit_batches_v1_thread_agent_created"
    " ON edit_batches_v1(thread_id, agent_id, created_at DESC);"
    "CREATE INDEX IF NOT EXISTS idx_edit_batches_v1_tool_call"
    " ON edit_batches_v1(tool_call_id);"
    "CREATE TABLE IF NOT EXISTS edit_file_mutations_v1 ("
    " thread_id TEXT NOT NULL,"
    " file_mutation_id TEXT NOT NULL,"
    " edit_batch_id TEXT NOT NULL,"
    " file_path TEXT NOT NULL DEFAULT '',"
    " ordinal_in_batch INTEGER NOT NULL DEFAULT 0,"
    " status TEXT NOT NULL DEFAULT 'Applied',"
    " mutation_json TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(thread_id, file_mutation_id),"
    " FOREIGN KEY(thread_id, edit_batch_id) REFERENCES edit_batches_v1(thread_id, edit_batch_id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_edit_file_mutations_v1_batch"
    " ON edit_file_mutations_v1(thread_id, edit_batch_id, ordinal_in_batch);"
    "CREATE INDEX IF NOT EXISTS idx_edit_file_mutations_v1_file"
    " ON edit_file_mutations_v1(thread_id, file_path);"
    "CREATE TABLE IF NOT EXISTS edit_undo_actions_v1 ("
    " thread_id TEXT NOT NULL,"
    " undo_action_id TEXT NOT NULL,"
    " requested_by_agent_id TEXT NOT NULL DEFAULT '',"
    " target_edit_batch_id TEXT NOT NULL DEFAULT '',"
    " created_at INTEGER NOT NULL DEFAULT 0,"
    " result_status TEXT NOT NULL DEFAULT 'Succeeded',"
    " result_json TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(thread_id, undo_action_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS edit_redo_actions_v1 ("
    " thread_id TEXT NOT NULL,"
    " redo_action_id TEXT NOT NULL,"
    " target_undo_action_id TEXT NOT NULL DEFAULT '',"
    " created_at INTEGER NOT NULL DEFAULT 0,"
    " result_json TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(thread_id, redo_action_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_edit_redo_actions_v1_target"
    " ON edit_redo_actions_v1(thread_id, target_undo_action_id);"
    "CREATE TABLE IF NOT EXISTS transcript_undo_actions_v1 ("
    " thread_id TEXT NOT NULL,"
    " undo_action_id TEXT NOT NULL,"
    " agent_id TEXT NOT NULL DEFAULT '',"
    " scope_type TEXT NOT NULL DEFAULT '',"
    " scope_arg_json TEXT NOT NULL DEFAULT '{}',"
    " created_at INTEGER NOT NULL DEFAULT 0,"
    " redo_available INTEGER NOT NULL DEFAULT 0,"
    " reason TEXT NOT NULL DEFAULT '',"
    " action_json TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(thread_id, undo_action_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS transcript_redo_payloads_v1 ("
    " thread_id TEXT NOT NULL,"
    " undo_action_id TEXT NOT NULL,"
    " ordinal INTEGER NOT NULL,"
    " payload_json TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(thread_id, undo_action_id, ordinal),"
    " FOREIGN KEY(thread_id, undo_action_id) REFERENCES transcript_undo_actions_v1(thread_id, undo_action_id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_transcript_redo_payloads_v1_lookup"
    " ON transcript_redo_payloads_v1(thread_id, undo_action_id, ordinal);";

const char* kSchemaSQL =
    "PRAGMA foreign_keys=ON;"
    "PRAGMA temp_store=MEMORY;";

std::optional<std::string> readSchemaMeta(sqlite3* db, const std::string& key) {
    Statement stmt(db,
                   "SELECT value FROM schema_meta WHERE key=?;",
                   "Failed to prepare schema meta read");
    bindText(stmt.get(), 1, key);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    return text ? std::optional<std::string>(text) : std::optional<std::string>("");
}

ThreadPermissionMode threadPermissionModeFromStoredString(
    const std::string& value) {
    if (value == "AlwaysAllow") {
        return ThreadPermissionMode::AlwaysAllow;
    }
    if (value == "DenyAll") {
        return ThreadPermissionMode::DenyAll;
    }
    return ThreadPermissionMode::Request;
}

void writeSchemaMeta(sqlite3* db, const std::string& key, const std::string& value) {
    Statement stmt(
        db,
        "INSERT INTO schema_meta(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
        "Failed to prepare schema meta write");
    bindText(stmt.get(), 1, key);
    bindText(stmt.get(), 2, value);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throwSqliteError(db, "Failed to write schema metadata");
    }
}

void ensureSchemaInfrastructure(sqlite3* db) {
    execSql(db, kLegacySchemaSQL,
            "Failed to initialize legacy thread database schema");
    execSql(db, kNormalizedSchemaSQL,
            "Failed to initialize normalized thread database schema");

    auto version = readSchemaMeta(db, "schema_version");
    if (!version.has_value()) {
        writeSchemaMeta(db, "schema_version", "1");
    }
    if (!readSchemaMeta(db, "migration_legacy_to_v2").has_value()) {
        writeSchemaMeta(db, "migration_legacy_to_v2", "pending");
    }
}

void execPrepared(sqlite3* db, const std::string& sql,
                  const std::function<void(sqlite3_stmt*)>& binder,
                  const std::string& context) {
    Statement stmt(db, sql, context);
    binder(stmt.get());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throwSqliteError(db, context);
    }
}

}


void deleteTodoItemsV2(sqlite3* db, const std::string& threadId,
                       const std::string& agentId) {
    execPrepared(db,
                 "DELETE FROM todo_items_v2 WHERE thread_id=? AND agent_id=?;",
                 [&](sqlite3_stmt* stmt) {
                     bindText(stmt, 1, threadId);
                     bindText(stmt, 2, agentId);
                 },
                 "Failed to delete normalized todo items");
}

void persistTodoItemsV2(sqlite3* db, const AgentTodoList& list) {
    for (const auto& item : list.items) {
        execPrepared(
            db,
            "INSERT INTO todo_items_v2(thread_id, agent_id, item_id, text, status, chunk_id, plan_id, created_at, updated_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);",
            [&](sqlite3_stmt* stmt) {
                bindText(stmt, 1, list.threadId);
                bindText(stmt, 2, list.agentId);
                sqlite3_bind_int(stmt, 3, item.id);
                bindText(stmt, 4, item.text);
                const auto itemJson = toJson(item);
                const auto status = itemJson.HasMember("status") && itemJson["status"].IsString()
                                        ? std::string(itemJson["status"].GetString())
                                        : std::string("Pending");
                bindText(stmt, 5, status);
                bindText(stmt, 6, item.chunkId);
                bindText(stmt, 7, item.planId);
                sqlite3_bind_int64(stmt, 8, static_cast<sqlite3_int64>(item.createdAt));
                sqlite3_bind_int64(stmt, 9, static_cast<sqlite3_int64>(item.updatedAt));
            },
            "Failed to write normalized todo item");
    }
}

sqlite3_int64 insertAgentTurnV2(sqlite3* db, const std::string& threadId,
                                const std::string& agentId,
                                const AgentTurn& turn) {
    const auto turnJson = toJson(turn);
    sqlite3_int64 rowId = 0;
    execPrepared(
        db,
        "INSERT INTO agent_turns_v2(thread_id, agent_id, turn_id, stop_reason, prompt_tokens, completion_tokens, reasoning_tokens, total_tokens, estimated_cost_usd) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);",
        [&](sqlite3_stmt* stmt) {
            bindText(stmt, 1, threadId);
            bindText(stmt, 2, agentId);
            bindText(stmt, 3, turn.turnId);
            const auto stopReason = turnJson.HasMember("stopReason") && turnJson["stopReason"].IsString()
                                        ? std::string(turnJson["stopReason"].GetString())
                                        : std::string("stop");
            bindText(stmt, 4, stopReason);
            sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(turn.metrics.tokens.prompt));
            sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(turn.metrics.tokens.completion));
            sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(turn.metrics.tokens.reasoning));
            sqlite3_bind_int64(stmt, 8, static_cast<sqlite3_int64>(turn.metrics.tokens.total));
            sqlite3_bind_double(stmt, 9, turn.metrics.estimatedCostUsd);
        },
        "Failed to write normalized agent turn");
    rowId = sqlite3_last_insert_rowid(db);
    return rowId;
}

void persistAgentTurnMessagesV2(sqlite3* db, sqlite3_int64 turnRowId,
                                const AgentTurn& turn) {
    int messageOrdinal = 0;
    for (const auto& msg : turn.messages) {
        execPrepared(
            db,
            "INSERT INTO turn_messages_v2(turn_row_id, message_id, role, visibility, timestamp, parent_id, ordinal) VALUES(?, ?, ?, ?, ?, ?, ?);",
            [&](sqlite3_stmt* stmt) {
                const auto msgJson = toJson(msg);
                sqlite3_bind_int64(stmt, 1, turnRowId);
                bindText(stmt, 2, msg.id);
                const auto role = msgJson.HasMember("role") && msgJson["role"].IsString()
                                      ? std::string(msgJson["role"].GetString())
                                      : std::string();
                const auto visibility = msgJson.HasMember("visibility") && msgJson["visibility"].IsString()
                                            ? std::string(msgJson["visibility"].GetString())
                                            : std::string();
                bindText(stmt, 3, role);
                bindText(stmt, 4, visibility);
                sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(msg.timestamp));
                if (msg.parentId.has_value()) bindText(stmt, 6, *msg.parentId);
                else sqlite3_bind_null(stmt, 6);
                sqlite3_bind_int(stmt, 7, messageOrdinal);
            },
            "Failed to write normalized turn message");
        const sqlite3_int64 messageRowId = sqlite3_last_insert_rowid(db);
        int partOrdinal = 0;
        for (const auto& part : msg.content) {
            execPrepared(
                db,
                "INSERT INTO message_parts_v2(message_row_id, ordinal, part_type, payload_json) VALUES(?, ?, ?, ?);",
                [&](sqlite3_stmt* stmt) {
                    const auto partJson = toJson(part);
                    sqlite3_bind_int64(stmt, 1, messageRowId);
                    sqlite3_bind_int(stmt, 2, partOrdinal);
                    const auto type = partJson.HasMember("type") && partJson["type"].IsString()
                                          ? std::string(partJson["type"].GetString())
                                          : std::string();
                    bindText(stmt, 3, type);
                    bindText(stmt, 4, firmius::shared::toJsonString(partJson));
                },
                "Failed to write normalized message part");
            partOrdinal++;
        }
        messageOrdinal++;
    }
}

void migrateLegacyToV2(sqlite3* db) {
    const auto state = readSchemaMeta(db, "migration_legacy_to_v2");
    if (state.has_value() && *state == "complete") {
        writeSchemaMeta(db, "schema_version", std::to_string(kCurrentSchemaVersion));
        return;
    }

    withImmediateTransaction(db, [&]() {
        writeSchemaMeta(db, "migration_legacy_to_v2", "in_progress");

        Statement threadStmt(
            db,
            "SELECT thread_id, metadata_json FROM threads ORDER BY created_at ASC, thread_id ASC;",
            "Failed to prepare legacy thread metadata migration read");
        while (sqlite3_step(threadStmt.get()) == SQLITE_ROW) {
            const auto* threadIdText = reinterpret_cast<const char*>(sqlite3_column_text(threadStmt.get(), 0));
            const auto* metadataText = reinterpret_cast<const char*>(sqlite3_column_text(threadStmt.get(), 1));
            if (!threadIdText || !metadataText) continue;
            try {
                const auto doc = parseJson(metadataText, "legacy thread metadata");
                ThreadMetadata metadata = threadMetadataFromJson(doc);
                if (metadata.threadId.empty()) metadata.threadId = threadIdText;
                execPrepared(
                    db,
                    "INSERT OR IGNORE INTO thread_metadata_v2(thread_id, title, host_type, host_identifier, cwd, lead_persona, is_benchmark_run, benchmark_id, benchmark_task_id, active_plan_id, permission_mode, created_at, last_active_at, host_options_json, retryable_request_json) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
                    [&](sqlite3_stmt* stmt) {
                        const auto metadataJson = toJson(metadata);
                        const auto hostOptionsJson = toJson(metadata.hostOptions);
                        bindText(stmt, 1, metadata.threadId);
                        bindText(stmt, 2, metadata.title);
                        const auto hostType = metadataJson.HasMember("hostOptions") && metadataJson["hostOptions"].IsObject() && metadataJson["hostOptions"].HasMember("type") && metadataJson["hostOptions"]["type"].IsString() ? std::string(metadataJson["hostOptions"]["type"].GetString()) : std::string();
                        bindText(stmt, 3, hostType);
                        bindText(stmt, 4, metadata.hostIdentifier);
                        bindText(stmt, 5, metadata.cwd);
                        bindText(stmt, 6, metadata.leadPersona);
                        sqlite3_bind_int(stmt, 7, metadata.isBenchmarkRun ? 1 : 0);
                        bindText(stmt, 8, metadata.benchmarkId);
                        bindText(stmt, 9, metadata.benchmarkTaskId);
                        bindText(stmt, 10, "");
                        const auto permissionMode = metadataJson.HasMember("permissionMode") && metadataJson["permissionMode"].IsString() ? std::string(metadataJson["permissionMode"].GetString()) : std::string("Request");
                        bindText(stmt, 11, permissionMode);
                        sqlite3_bind_int64(stmt, 12, static_cast<sqlite3_int64>(metadata.createdAt));
                        sqlite3_bind_int64(stmt, 13, static_cast<sqlite3_int64>(metadata.lastActiveAt));
                        bindText(stmt, 14, firmius::shared::toJsonString(hostOptionsJson));
                        if (metadataJson.HasMember("lastRetryableRequest") && !metadataJson["lastRetryableRequest"].IsNull()) {
                            rapidjson::Document retryDoc;
                            retryDoc.CopyFrom(metadataJson["lastRetryableRequest"], retryDoc.GetAllocator());
                            bindText(stmt, 15, firmius::shared::toJsonString(retryDoc));
                        } else {
                            sqlite3_bind_null(stmt, 15);
                        }
                    },
                    "Failed to write migrated thread metadata row");
            } catch (...) {
            }
        }


        Statement todoStmt(
            db,
            "SELECT thread_id, agent_id, todo_json FROM agent_todos ORDER BY thread_id ASC, agent_id ASC;",
            "Failed to prepare legacy todo migration read");
        while (sqlite3_step(todoStmt.get()) == SQLITE_ROW) {
            const auto* threadIdText = reinterpret_cast<const char*>(sqlite3_column_text(todoStmt.get(), 0));
            const auto* agentIdText = reinterpret_cast<const char*>(sqlite3_column_text(todoStmt.get(), 1));
            const auto* todoJsonText = reinterpret_cast<const char*>(sqlite3_column_text(todoStmt.get(), 2));
            if (!threadIdText || !agentIdText || !todoJsonText) continue;
            try {
                auto d = parseJson(todoJsonText, "legacy todo");
                AgentTodoList list = agentTodoListFromJson(d);
                if (list.threadId.empty()) list.threadId = threadIdText;
                if (list.agentId.empty()) list.agentId = agentIdText;
                execPrepared(
                    db,
                    "INSERT OR IGNORE INTO agent_todos_v2(thread_id, agent_id, next_id) VALUES(?, ?, ?);",
                    [&](sqlite3_stmt* stmt) {
                        bindText(stmt, 1, list.threadId);
                        bindText(stmt, 2, list.agentId);
                        sqlite3_bind_int(stmt, 3, list.nextId);
                    },
                    "Failed to write migrated todo header");
                deleteTodoItemsV2(db, list.threadId, list.agentId);
                persistTodoItemsV2(db, list);
            } catch (...) {
            }
        }

        Statement turnStmt(
            db,
            "SELECT thread_id, agent_id, turn_json FROM agent_turns ORDER BY id ASC;",
            "Failed to prepare legacy agent turn migration read");
        while (sqlite3_step(turnStmt.get()) == SQLITE_ROW) {
            const auto* threadIdText = reinterpret_cast<const char*>(sqlite3_column_text(turnStmt.get(), 0));
            const auto* agentIdText = reinterpret_cast<const char*>(sqlite3_column_text(turnStmt.get(), 1));
            const auto* turnJsonText = reinterpret_cast<const char*>(sqlite3_column_text(turnStmt.get(), 2));
            if (!threadIdText || !agentIdText || !turnJsonText) continue;
            try {
                auto d = parseJson(turnJsonText, "legacy turn");
                AgentTurn turn = agentTurnFromJsonValue(d);
                const sqlite3_int64 rowId = insertAgentTurnV2(db, threadIdText, agentIdText, turn);
                persistAgentTurnMessagesV2(db, rowId, turn);
            } catch (...) {
            }
        }

        Statement manifestStmt(
            db,
            "SELECT thread_id, agent_manifest_json FROM thread_states WHERE agent_manifest_json IS NOT NULL ORDER BY thread_id ASC;",
            "Failed to prepare legacy agent manifest migration read");
        while (sqlite3_step(manifestStmt.get()) == SQLITE_ROW) {
            const auto* threadIdText = reinterpret_cast<const char*>(sqlite3_column_text(manifestStmt.get(), 0));
            const auto* manifestJsonText = reinterpret_cast<const char*>(sqlite3_column_text(manifestStmt.get(), 1));
            if (!threadIdText || !manifestJsonText) continue;
            try {
                auto d = parseJson(manifestJsonText, "legacy agent manifest");
                if (!d.IsObject()) {
                    continue;
                }

                int legacyRootAgents = 0;
                int v2RootAgents = 0;
                int legacyTurnsCovered = 0;
                int v2TurnsCovered = 0;

                for (auto it = d.MemberBegin(); it != d.MemberEnd(); ++it) {
                    if (!it->name.IsString() || !it->value.IsObject()) continue;
                    std::string parentId;
                    if (it->value.HasMember("parentId") && it->value["parentId"].IsString()) {
                        parentId = it->value["parentId"].GetString();
                    }
                    if (parentId.empty()) {
                        legacyRootAgents++;
                    }

                    Statement turnCountStmt(
                        db,
                        "SELECT COUNT(*) FROM agent_turns_v2 WHERE thread_id=? AND agent_id=?;",
                        "Failed to prepare migrated legacy turn count query");
                    bindText(turnCountStmt.get(), 1, threadIdText);
                    bindText(turnCountStmt.get(), 2, it->name.GetString());
                    if (sqlite3_step(turnCountStmt.get()) == SQLITE_ROW) {
                        legacyTurnsCovered += sqlite3_column_int(turnCountStmt.get(), 0);
                    }
                }

                Statement v2ManifestStmt(
                    db,
                    "SELECT agent_id, parent_id FROM agent_manifest_v2 WHERE thread_id=? ORDER BY agent_id ASC;",
                    "Failed to prepare v2 manifest comparison query");
                bindText(v2ManifestStmt.get(), 1, threadIdText);
                while (sqlite3_step(v2ManifestStmt.get()) == SQLITE_ROW) {
                    const auto* agentId = reinterpret_cast<const char*>(sqlite3_column_text(v2ManifestStmt.get(), 0));
                    const auto* parentId = reinterpret_cast<const char*>(sqlite3_column_text(v2ManifestStmt.get(), 1));
                    if (!parentId || *parentId == '\0') {
                        v2RootAgents++;
                    }
                    if (agentId) {
                        Statement turnCountStmt(
                            db,
                            "SELECT COUNT(*) FROM agent_turns_v2 WHERE thread_id=? AND agent_id=?;",
                            "Failed to prepare migrated v2 turn count query");
                        bindText(turnCountStmt.get(), 1, threadIdText);
                        bindText(turnCountStmt.get(), 2, agentId);
                        if (sqlite3_step(turnCountStmt.get()) == SQLITE_ROW) {
                            v2TurnsCovered += sqlite3_column_int(turnCountStmt.get(), 0);
                        }
                    }
                }

                if (legacyRootAgents > 0 && (v2RootAgents == 0 || legacyTurnsCovered > v2TurnsCovered)) {
                    execPrepared(db,
                                 "DELETE FROM agent_manifest_v2 WHERE thread_id=?;",
                                 [&](sqlite3_stmt* stmt) { bindText(stmt, 1, threadIdText); },
                                 "Failed to clear migrated agent manifest");
                }

                for (auto it = d.MemberBegin(); it != d.MemberEnd(); ++it) {
                    if (!it->name.IsString() || !it->value.IsObject()) continue;
                    AgentManifestEntry entry;
                    if (it->value.HasMember("persona") && it->value["persona"].IsString()) entry.persona = it->value["persona"].GetString();
                    if (it->value.HasMember("parentId") && it->value["parentId"].IsString()) entry.parentId = it->value["parentId"].GetString();
                    if (it->value.HasMember("friendlyName") && it->value["friendlyName"].IsString()) entry.friendlyName = it->value["friendlyName"].GetString();
                    if (it->value.HasMember("title") && it->value["title"].IsString()) entry.title = it->value["title"].GetString();
                    if (it->value.HasMember("persistHistory") && it->value["persistHistory"].IsBool()) entry.persistHistory = it->value["persistHistory"].GetBool();
                    execPrepared(
                        db,
                        "INSERT OR REPLACE INTO agent_manifest_v2(thread_id, agent_id, persona, parent_id, friendly_name, title, persist_history) VALUES(?, ?, ?, ?, ?, ?, ?);",
                        [&](sqlite3_stmt* stmt) {
                            bindText(stmt, 1, threadIdText);
                            bindText(stmt, 2, it->name.GetString());
                            bindText(stmt, 3, entry.persona);
                            bindText(stmt, 4, entry.parentId);
                            bindText(stmt, 5, entry.friendlyName);
                            bindText(stmt, 6, entry.title);
                            sqlite3_bind_int(stmt, 7, entry.persistHistory ? 1 : 0);
                        },
                        "Failed to write migrated agent manifest entry");
                }
            } catch (...) {
            }
        }

        writeSchemaMeta(db, "migration_legacy_to_v2", "complete");
        writeSchemaMeta(db, "schema_version", std::to_string(kCurrentSchemaVersion));
    });
}


struct SqliteConnection {
    explicit SqliteConnection(const std::string& dbPath)
        : dbPath_(dbPath) {
        openAndConfigure();
    }

    ~SqliteConnection() {
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    }

    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    /// Re-open the connection if the underlying handle went bad.
    void ensureValid() {
        if (db) {
            // Quick health check – a failed PRAGMA is a sign the fd is dead.
            char* err = nullptr;
            const int rc = sqlite3_exec(db, "PRAGMA quick_check(1);",
                                        nullptr, nullptr, &err);
            if (err) sqlite3_free(err);
            if (rc == SQLITE_OK) return;

            // Connection is broken – tear it down and reopen.
            sqlite3_close(db);
            db = nullptr;
        }
        openAndConfigure();
    }

    sqlite3* db = nullptr;

private:
    std::string dbPath_;

    void openAndConfigure() {
        std::string lastErr;
        for (int attempt = 1; attempt <= kConnMaxRetries; ++attempt) {
            if (sqlite3_open_v2(dbPath_.c_str(), &db,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                    SQLITE_OPEN_FULLMUTEX,
                                nullptr) != SQLITE_OK) {
                lastErr = db ? sqlite3_errmsg(db) : "unknown sqlite error";
                if (db) {
                    sqlite3_close(db);
                    db = nullptr;
                }
                if (attempt < kConnMaxRetries) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kConnRetryBaseMs * attempt));
                    continue;
                }
                throw std::runtime_error(
                    "Failed to open thread database (" +
                    std::to_string(kConnMaxRetries) + " attempts): " + lastErr);
            }

            try {
                execSql(db, "PRAGMA busy_timeout=" + std::to_string(kSqliteBusyTimeoutMs) + ";",
                        "Failed to set sqlite busy timeout");
                sqlite3_exec(db, "PRAGMA journal_mode=WAL;",
                             nullptr, nullptr, nullptr);
                execSql(db, "PRAGMA synchronous=NORMAL;",
                        "Failed to configure sqlite synchronous mode");
                execSql(db, "PRAGMA temp_store=MEMORY;",
                        "Failed to configure sqlite temp store");
                execSql(db, "PRAGMA foreign_keys=ON;",
                        "Failed to enable sqlite foreign keys");
                execSql(db, kSchemaSQL,
                        "Failed to initialize thread database pragmas");
                ensureSchemaInfrastructure(db);
                migrateLegacyToV2(db);
                return; // success
            } catch (const std::exception& ex) {
                lastErr = ex.what();
                sqlite3_close(db);
                db = nullptr;
                if (attempt < kConnMaxRetries) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kConnRetryBaseMs * attempt));
                    continue;
                }
                throw;
            }
        }
    }
};

std::shared_ptr<SqliteConnection> acquireConnection(const std::string& basePath) {
    const std::string dbPath = dbPathForBase(basePath);
    std::filesystem::create_directories(basePath);
    return std::make_shared<SqliteConnection>(dbPath);
}

void ensureThreadExists(sqlite3* db, const std::string& threadId) {
    Statement stmt(db, "SELECT 1 FROM thread_metadata_v2 WHERE thread_id=? LIMIT 1;",
                   "Failed to prepare thread existence query");
    bindText(stmt.get(), 1, threadId);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return;
    }
    throw std::runtime_error("Thread not found: " + threadId);
}

bool agentHasActiveLiveRun(const std::string& threadId,
                           const std::string& agentId) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (!agent) {
        return false;
    }

    const auto& ctx = agent->getContext();
    if (!ctx.history || ctx.history->threadId != threadId) {
        return false;
    }

    return agent->isRunning() || agent->isBooting();
}

rapidjson::Document liveStateToJson(const AgentLiveState& liveState) {
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("thread_id", rapidjson::Value(liveState.threadId.c_str(), alloc),
                  alloc);
    doc.AddMember("agent_id", rapidjson::Value(liveState.agentId.c_str(), alloc),
                  alloc);
    return doc;
}

AgentLiveState liveStateFromJson(const rapidjson::Value& value) {
    AgentLiveState liveState;
    if (!value.IsObject()) {
        return liveState;
    }

    if (value.HasMember("thread_id") && value["thread_id"].IsString()) {
        liveState.threadId = value["thread_id"].GetString();
    }
    if (value.HasMember("agent_id") && value["agent_id"].IsString()) {
        liveState.agentId = value["agent_id"].GetString();
    }
    return liveState;
}



rapidjson::Value compactionSnapshotToJson(
    const CompactionSnapshot& snapshot, rapidjson::Document::AllocatorType& a) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("compactionId", rapidjson::Value(snapshot.compactionId.c_str(), a),
                a);
    v.AddMember("threadId", rapidjson::Value(snapshot.threadId.c_str(), a), a);
    v.AddMember("agentId", rapidjson::Value(snapshot.agentId.c_str(), a), a);
    v.AddMember("previousContextSize", snapshot.previousContextSize, a);
    v.AddMember("createdAt", snapshot.createdAt, a);
    rapidjson::Value turnsArray(rapidjson::kArrayType);
    for (const auto& turn : snapshot.turns) {
        rapidjson::Document turnDoc = toJson(turn);
        rapidjson::Value turnValue;
        turnValue.CopyFrom(turnDoc, a);
        turnsArray.PushBack(turnValue, a);
    }
    v.AddMember("turns", turnsArray, a);
    return v;
}

CompactionSnapshot compactionSnapshotFromJsonValue(const rapidjson::Value& v) {
    CompactionSnapshot snapshot;
    if (v.HasMember("compactionId") && v["compactionId"].IsString()) {
        snapshot.compactionId = v["compactionId"].GetString();
    }
    if (v.HasMember("threadId") && v["threadId"].IsString()) {
        snapshot.threadId = v["threadId"].GetString();
    }
    if (v.HasMember("agentId") && v["agentId"].IsString()) {
        snapshot.agentId = v["agentId"].GetString();
    }
    if (v.HasMember("previousContextSize") && v["previousContextSize"].IsUint()) {
        snapshot.previousContextSize = v["previousContextSize"].GetUint();
    }
    if (v.HasMember("createdAt") && v["createdAt"].IsUint64()) {
        snapshot.createdAt = v["createdAt"].GetUint64();
    }
    if (v.HasMember("turns") && v["turns"].IsArray()) {
        for (const auto& turn : v["turns"].GetArray()) {
            snapshot.turns.push_back(agentTurnFromJsonValue(turn));
        }
    }
    return snapshot;
}

const char* severityToString(CommandSeverity severity) {
    switch (severity) {
    case CommandSeverity::LOW:
        return "LOW";
    case CommandSeverity::MEDIUM:
        return "MEDIUM";
    case CommandSeverity::HIGH:
        return "HIGH";
    case CommandSeverity::VULNERABLE:
        return "VULNERABLE";
    }
    return "LOW";
}

CommandSeverity severityFromString(const std::string& value) {
    if (value == "VULNERABLE") {
        return CommandSeverity::VULNERABLE;
    }
    if (value == "HIGH") {
        return CommandSeverity::HIGH;
    }
    if (value == "MEDIUM") {
        return CommandSeverity::MEDIUM;
    }
    return CommandSeverity::LOW;
}

std::string stateFieldForColumn(const std::string& column) {
    if (column == "agent_manifest_json" || column == "permission_rules_json" ||
        column == "fleet_state_json") {
        return column;
    }
    throw std::runtime_error("Invalid thread state column");
}

std::optional<std::string> readThreadStateField(sqlite3* db,
                                                const std::string& threadId,
                                                const std::string& column) {
    Statement stmt(db,
                   "SELECT " + stateFieldForColumn(column) +
                       " FROM thread_states WHERE thread_id=?;",
                   "Failed to prepare thread state read");
    bindText(stmt.get(), 1, threadId);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    if (sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL) {
        return std::nullopt;
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    return text ? std::optional<std::string>(text) : std::nullopt;
}

void writeThreadStateField(sqlite3* db, const std::string& threadId,
                           const std::string& column,
                           const std::optional<std::string>& value) {
    const std::string c = stateFieldForColumn(column);
    Statement stmt(
        db,
        "INSERT INTO thread_states(thread_id, " + c + ") VALUES(?, ?) "
        "ON CONFLICT(thread_id) DO UPDATE SET " + c + "=excluded." + c + ";",
        "Failed to prepare thread state write");
    bindText(stmt.get(), 1, threadId);
    bindOptionalText(stmt.get(), 2, value);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throwSqliteError(db, "Failed to write thread state");
    }
}

void validateArtifactFilename(const std::string& filename) {
    if (filename.empty()) {
        throw std::runtime_error("Artifact filename cannot be empty");
    }
    if (filename == "." || filename == "..") {
        throw std::runtime_error("Artifact filename is invalid: " + filename);
    }
    if (filename.find('\0') != std::string::npos) {
        throw std::runtime_error("Artifact filename contains NUL byte");
    }
    if (filename.find("..") != std::string::npos) {
        throw std::runtime_error("Artifact filename cannot contain '..'");
    }
    if (filename.front() == '/' || filename.front() == '\\') {
        throw std::runtime_error("Artifact filename must be relative");
    }
    if (filename.find('\\') != std::string::npos) {
        throw std::runtime_error("Artifact filename cannot contain '\\\\'");
    }
}

std::shared_ptr<std::mutex> acquireTodoMutex(const std::string& threadId,
                                             const std::string& agentId) {
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> registry;
    const std::string key = threadId + ":" + agentId;
    std::lock_guard<std::mutex> guard(registryMutex);
    if (auto existing = registry[key].lock()) {
        return existing;
    }
    auto created = std::make_shared<std::mutex>();
    registry[key] = created;
    return created;
}

} // namespace

namespace firmius::core {

using namespace firmius::shared;

std::string ThreadManager::defaultBasePath() {
    const std::filesystem::path sharedHomePath =
        firmius::shared::PlatformPaths::firmiusHomeDir() / "threads";
    if (ensureWritableDirectory(sharedHomePath)) {
        return sharedHomePath.string();
    }

    const std::filesystem::path localPath =
        std::filesystem::current_path() / ".firmius" / "threads";
    if (ensureWritableDirectory(localPath)) {
        return localPath.string();
    }

    const std::filesystem::path tempPath =
        std::filesystem::temp_directory_path() / "firmius" / "threads";
    ensureWritableDirectory(tempPath);
    return tempPath.string();
}

std::string ThreadManager::threadDirectoryPath(const std::string& basePath,
                                               const std::string& threadId) {
    return (std::filesystem::path(basePath) / threadId).string();
}

std::string ThreadManager::compactionSnapshotPath(const std::string& basePath,
                                                  const std::string& threadId,
                                                  const std::string& agentId) {
    return threadDirectoryPath(basePath, threadId) + "/compaction_" + agentId +
           ".json";
}

ThreadManager::ThreadManager(std::string basePath)
    : basePath_(std::move(basePath)) {
    std::filesystem::create_directories(basePath_);
    (void)acquireConnection(basePath_);
}

std::vector<std::string> ThreadManager::listThreads() const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT thread_id FROM thread_metadata_v2 ORDER BY created_at ASC, thread_id ASC;",
                   "Failed to prepare list threads query");
    std::vector<std::string> threads;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        if (text) {
            threads.emplace_back(text);
        }
    }
    return threads;
}

std::string ThreadManager::createThread(const ThreadMetadata& metadata) {
    auto conn = acquireConnection(basePath_);

    ThreadMetadata persisted = metadata;
    persisted.threadId = StringUtil::generateUuid();
    persisted.createdAt = nowEpochMs();
    persisted.lastActiveAt = persisted.createdAt;

    std::filesystem::create_directories(
        std::filesystem::path(basePath_) / persisted.threadId);

    const auto metadataJson = toJson(persisted);
    const auto hostOptionsJson = toJson(persisted.hostOptions);
    execPrepared(
        conn->db,
        "INSERT INTO thread_metadata_v2(thread_id, title, host_type, host_identifier, cwd, lead_persona, is_benchmark_run, benchmark_id, benchmark_task_id, active_plan_id, permission_mode, created_at, last_active_at, host_options_json, retryable_request_json) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        [&](sqlite3_stmt* stmt) {
            bindText(stmt, 1, persisted.threadId);
            bindText(stmt, 2, persisted.title);
            const auto hostType = metadataJson.HasMember("hostOptions") &&
                                          metadataJson["hostOptions"].IsObject() &&
                                          metadataJson["hostOptions"].HasMember("type") &&
                                          metadataJson["hostOptions"]["type"].IsString()
                                      ? std::string(metadataJson["hostOptions"]["type"].GetString())
                                      : std::string();
            bindText(stmt, 3, hostType);
            bindText(stmt, 4, persisted.hostIdentifier);
            bindText(stmt, 5, persisted.cwd);
            bindText(stmt, 6, persisted.leadPersona);
            sqlite3_bind_int(stmt, 7, persisted.isBenchmarkRun ? 1 : 0);
            bindText(stmt, 8, persisted.benchmarkId);
            bindText(stmt, 9, persisted.benchmarkTaskId);
            bindText(stmt, 10, "");
            const auto permissionMode = metadataJson.HasMember("permissionMode") &&
                                                metadataJson["permissionMode"].IsString()
                                            ? std::string(metadataJson["permissionMode"].GetString())
                                            : std::string("Request");
            bindText(stmt, 11, permissionMode);
            sqlite3_bind_int64(stmt, 12, static_cast<sqlite3_int64>(persisted.createdAt));
            sqlite3_bind_int64(stmt, 13, static_cast<sqlite3_int64>(persisted.lastActiveAt));
            bindText(stmt, 14, firmius::shared::toJsonString(hostOptionsJson));
            if (metadataJson.HasMember("lastRetryableRequest") &&
                !metadataJson["lastRetryableRequest"].IsNull()) {
                rapidjson::Document retryDoc;
                retryDoc.CopyFrom(metadataJson["lastRetryableRequest"], retryDoc.GetAllocator());
                bindText(stmt, 15, firmius::shared::toJsonString(retryDoc));
            } else {
                sqlite3_bind_null(stmt, 15);
            }
        },
        "Failed to create thread");

    return persisted.threadId;
}

ThreadMetadata ThreadManager::getMetadata(const std::string& threadId) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT title, host_type, host_identifier, cwd, lead_persona, is_benchmark_run, benchmark_id, benchmark_task_id, active_plan_id, permission_mode, created_at, last_active_at, host_options_json, retryable_request_json FROM thread_metadata_v2 WHERE thread_id=?;",
                   "Failed to prepare thread metadata query");
    bindText(stmt.get(), 1, threadId);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error("Thread not found: " + threadId);
    }

    ThreadMetadata meta;
    meta.threadId = threadId;
    const auto* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    const auto* hostIdentifier = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    const auto* cwd = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    const auto* leadPersona = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
    const auto* benchmarkId = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
    const auto* benchmarkTaskId = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
    const auto* permissionMode = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 9));
    const auto* hostOptionsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 12));
    const auto* retryableRequestJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 13));
    meta.title = title ? title : "Untitled Thread";
    meta.hostIdentifier = hostIdentifier ? hostIdentifier : "";
    meta.cwd = cwd ? cwd : "";
    meta.leadPersona = leadPersona ? leadPersona : "";
    meta.isBenchmarkRun = sqlite3_column_int(stmt.get(), 5) != 0;
    meta.benchmarkId = benchmarkId ? benchmarkId : "";
    meta.benchmarkTaskId = benchmarkTaskId ? benchmarkTaskId : "";
    meta.permissionMode = permissionMode ? threadPermissionModeFromStoredString(permissionMode)
                                         : ThreadPermissionMode::Request;
    meta.createdAt = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 10));
    meta.lastActiveAt = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 11));
    if (hostOptionsJson && *hostOptionsJson) {
        auto d = parseJson(hostOptionsJson, "thread host options");
        meta.hostOptions = hostCreationOptionsFromJsonValue(d);
    }
    if (retryableRequestJson && *retryableRequestJson) {
        auto d = parseJson(retryableRequestJson, "thread retryable request");
        if (d.IsObject()) {
            ThreadMetadata::RetryableRequest retry;
            retry.targetAgentId = d.HasMember("targetAgentId") && d["targetAgentId"].IsString() ? d["targetAgentId"].GetString() : "";
            retry.turnId = d.HasMember("turnId") && d["turnId"].IsString() ? d["turnId"].GetString() : "";
            retry.text = d.HasMember("text") && d["text"].IsString() ? d["text"].GetString() : "";
            retry.recordedAt = d.HasMember("recordedAt") && d["recordedAt"].IsUint64() ? d["recordedAt"].GetUint64() : 0;
            retry.eligible = d.HasMember("eligible") && d["eligible"].IsBool() ? d["eligible"].GetBool() : false;
            if (d.HasMember("images") && d["images"].IsArray()) {
                for (const auto& imageValue : d["images"].GetArray()) {
                    try {
                        auto part = messagePartFromJsonValue(imageValue);
                        if (auto* image = std::get_if<ImageContent>(&part)) {
                            retry.images.push_back(*image);
                        }
                    } catch (...) {
                    }
                }
            }
            if (!retry.text.empty() || !retry.images.empty()) {
                meta.lastRetryableRequest = std::move(retry);
            }
        }
    }
    return meta;
}

bool ThreadManager::tryGetMetadata(const std::string& threadId,
                                   ThreadMetadata& metadata,
                                   std::string* error) const {
    try {
        metadata = getMetadata(threadId);
        return true;
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
    } catch (...) {
        if (error) {
            *error = "Unknown error loading thread metadata";
        }
    }
    return false;
}

AgentHistory ThreadManager::loadAgentHistory(const std::string& threadId,
                                             const std::string& agentId) const {
    auto conn = acquireConnection(basePath_);

    AgentHistory history;
    history.threadId = threadId;

    Statement turnStmt(conn->db,
                       "SELECT id, turn_id, stop_reason, prompt_tokens, completion_tokens, reasoning_tokens, total_tokens, estimated_cost_usd FROM agent_turns_v2 WHERE thread_id=? AND agent_id=? ORDER BY id ASC;",
                       "Failed to prepare normalized agent history query");
    bindText(turnStmt.get(), 1, threadId);
    bindText(turnStmt.get(), 2, agentId);

    while (sqlite3_step(turnStmt.get()) == SQLITE_ROW) {
        AgentTurn turn;
        const sqlite3_int64 turnRowId = sqlite3_column_int64(turnStmt.get(), 0);
        const auto* turnId = reinterpret_cast<const char*>(sqlite3_column_text(turnStmt.get(), 1));
        const auto* stopReason = reinterpret_cast<const char*>(sqlite3_column_text(turnStmt.get(), 2));
        turn.turnId = turnId ? turnId : "";
        turn.metrics.tokens.prompt = static_cast<uint64_t>(sqlite3_column_int64(turnStmt.get(), 3));
        turn.metrics.tokens.completion = static_cast<uint64_t>(sqlite3_column_int64(turnStmt.get(), 4));
        turn.metrics.tokens.reasoning = static_cast<uint64_t>(sqlite3_column_int64(turnStmt.get(), 5));
        turn.metrics.tokens.total = static_cast<uint64_t>(sqlite3_column_int64(turnStmt.get(), 6));
        turn.metrics.estimatedCostUsd = sqlite3_column_double(turnStmt.get(), 7);
        if (stopReason) {
            auto turnJson = toJson(turn);
            turnJson.AddMember("stopReason", rapidjson::Value(stopReason, turnJson.GetAllocator()), turnJson.GetAllocator());
            turn = agentTurnFromJsonValue(turnJson);
        }

        Statement msgStmt(conn->db,
                          "SELECT id, message_id, role, visibility, timestamp, parent_id FROM turn_messages_v2 WHERE turn_row_id=? ORDER BY ordinal ASC;",
                          "Failed to prepare normalized turn messages query");
        sqlite3_bind_int64(msgStmt.get(), 1, turnRowId);
        while (sqlite3_step(msgStmt.get()) == SQLITE_ROW) {
            Message msg;
            const sqlite3_int64 messageRowId = sqlite3_column_int64(msgStmt.get(), 0);
            const auto* messageId = reinterpret_cast<const char*>(sqlite3_column_text(msgStmt.get(), 1));
            const auto* role = reinterpret_cast<const char*>(sqlite3_column_text(msgStmt.get(), 2));
            const auto* visibility = reinterpret_cast<const char*>(sqlite3_column_text(msgStmt.get(), 3));
            const auto* parentId = reinterpret_cast<const char*>(sqlite3_column_text(msgStmt.get(), 5));
            msg.id = messageId ? messageId : "";
            msg.timestamp = static_cast<uint64_t>(sqlite3_column_int64(msgStmt.get(), 4));
            if (parentId) msg.parentId = std::string(parentId);
            rapidjson::Document msgJson;
            msgJson.SetObject();
            auto& a = msgJson.GetAllocator();
            msgJson.AddMember("id", rapidjson::Value(msg.id.c_str(), a), a);
            msgJson.AddMember("role", rapidjson::Value(role ? role : "assistant", a), a);
            msgJson.AddMember("visibility", rapidjson::Value(visibility ? visibility : "visible", a), a);
            rapidjson::Value content(rapidjson::kArrayType);
            msgJson.AddMember("content", content, a);
            msgJson.AddMember("timestamp", msg.timestamp, a);
            if (msg.parentId.has_value()) msgJson.AddMember("parentId", rapidjson::Value(msg.parentId->c_str(), a), a);
            else msgJson.AddMember("parentId", rapidjson::Value(rapidjson::kNullType), a);
            msg = messageFromJsonValue(msgJson);

            Statement partStmt(conn->db,
                               "SELECT payload_json FROM message_parts_v2 WHERE message_row_id=? ORDER BY ordinal ASC;",
                               "Failed to prepare normalized message parts query");
            sqlite3_bind_int64(partStmt.get(), 1, messageRowId);
            while (sqlite3_step(partStmt.get()) == SQLITE_ROW) {
                const auto* payload = reinterpret_cast<const char*>(sqlite3_column_text(partStmt.get(), 0));
                if (!payload) continue;
                try {
                    auto payloadDoc = parseJson(payload, "message part payload");
                    msg.content.push_back(messagePartFromJsonValue(payloadDoc));
                } catch (...) {
                }
            }
            turn.messages.push_back(std::move(msg));
        }
        history.turns.push_back(std::move(turn));
    }

    std::unordered_map<std::string, bool> toolCallIds;
    std::unordered_map<std::string, bool> toolResultIds;

    for (const auto& turn : history.turns) {
        for (const auto& msg : turn.messages) {
            for (const auto& part : msg.content) {
                if (auto* tc = std::get_if<ToolCallContent>(&part)) {
                    if (!tc->id.empty()) {
                        toolCallIds[tc->id] = true;
                    }
                } else if (auto* tr = std::get_if<ToolResultContent>(&part)) {
                    if (!tr->toolCallId.empty()) {
                        toolResultIds[tr->toolCallId] = true;
                    }
                }
            }
        }
    }

    std::vector<std::string> orphanedCalls;
    for (const auto& [callId, _] : toolCallIds) {
        if (toolResultIds.find(callId) == toolResultIds.end()) {
            orphanedCalls.push_back(callId);
        }
    }

    if (!orphanedCalls.empty() && !agentHasActiveLiveRun(threadId, agentId)) {
        AgentTurn repairTurn;
        repairTurn.turnId = "auto-repair-" + std::to_string(nowEpochMs());
        repairTurn.stopReason = StopReason::Stop;

        for (const auto& callId : orphanedCalls) {
            Message repairMsg;
            repairMsg.role = Role::ToolResult;
            repairMsg.timestamp = nowEpochMs();

            ToolResultContent repairResult;
            repairResult.toolCallId = callId;
            repairResult.result = "User aborted tool manually.";
            repairResult.success = false;

            repairMsg.content.push_back(repairResult);
            repairTurn.messages.push_back(repairMsg);
        }

        history.turns.push_back(std::move(repairTurn));
    }

    return history;
}

std::vector<std::string> ThreadManager::listAgents(const std::string& threadId) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT DISTINCT agent_id FROM agent_turns_v2 WHERE thread_id=? ORDER BY agent_id ASC;",
                   "Failed to prepare list agents query");
    bindText(stmt.get(), 1, threadId);

    std::vector<std::string> agents;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        if (text) {
            agents.emplace_back(text);
        }
    }
    return agents;
}

void ThreadManager::updateHostIdentifier(const std::string& threadId,
                                         const std::string& hostIdentifier) {
    auto metadata = getMetadata(threadId);
    metadata.hostIdentifier = hostIdentifier;
    updateMetadata(threadId, metadata);
}

void ThreadManager::deleteThread(const std::string& threadId) {
    auto conn = acquireConnection(basePath_);
    withImmediateTransaction(conn->db, [&]() {
        const std::vector<std::string> deletes = {
            "DELETE FROM threads WHERE thread_id=?;",
            "DELETE FROM thread_states WHERE thread_id=?;",
            "DELETE FROM agent_turns WHERE thread_id=?;",
            "DELETE FROM agent_todos WHERE thread_id=?;",
            "DELETE FROM agent_live_state WHERE thread_id=?;",
            "DELETE FROM compaction_snapshots WHERE thread_id=?;",
            "DELETE FROM artifacts WHERE thread_id=?;",
        };
        for (const auto& sql : deletes) {
            Statement stmt(conn->db, sql, "Failed to prepare thread delete");
            bindText(stmt.get(), 1, threadId);
            if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
                throwSqliteError(conn->db, "Failed to delete thread data");
            }
        }
    });

    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(basePath_) / threadId, ec);
}

void ThreadManager::updateMetadata(const std::string& threadId,
                                   const ThreadMetadata& metadata) {
    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);

    ThreadMetadata persisted = metadata;
    if (persisted.threadId.empty()) {
        persisted.threadId = threadId;
    }
    const auto metadataJson = toJson(persisted);
    const auto hostOptionsJson = toJson(persisted.hostOptions);
    execPrepared(
        conn->db,
        "UPDATE thread_metadata_v2 SET title=?, host_type=?, host_identifier=?, cwd=?, lead_persona=?, is_benchmark_run=?, benchmark_id=?, benchmark_task_id=?, active_plan_id=?, permission_mode=?, created_at=?, last_active_at=?, host_options_json=?, retryable_request_json=? WHERE thread_id=?;",
        [&](sqlite3_stmt* stmt) {
            bindText(stmt, 1, persisted.title);
            const auto hostType = metadataJson.HasMember("hostOptions") &&
                                          metadataJson["hostOptions"].IsObject() &&
                                          metadataJson["hostOptions"].HasMember("type") &&
                                          metadataJson["hostOptions"]["type"].IsString()
                                      ? std::string(metadataJson["hostOptions"]["type"].GetString())
                                      : std::string();
            bindText(stmt, 2, hostType);
            bindText(stmt, 3, persisted.hostIdentifier);
            bindText(stmt, 4, persisted.cwd);
            bindText(stmt, 5, persisted.leadPersona);
            sqlite3_bind_int(stmt, 6, persisted.isBenchmarkRun ? 1 : 0);
            bindText(stmt, 7, persisted.benchmarkId);
            bindText(stmt, 8, persisted.benchmarkTaskId);
            bindText(stmt, 9, "");
            const auto permissionMode = metadataJson.HasMember("permissionMode") &&
                                                metadataJson["permissionMode"].IsString()
                                            ? std::string(metadataJson["permissionMode"].GetString())
                                            : std::string("Request");
            bindText(stmt, 10, permissionMode);
            sqlite3_bind_int64(stmt, 11, static_cast<sqlite3_int64>(persisted.createdAt));
            sqlite3_bind_int64(stmt, 12, static_cast<sqlite3_int64>(persisted.lastActiveAt));
            bindText(stmt, 13, firmius::shared::toJsonString(hostOptionsJson));
            if (metadataJson.HasMember("lastRetryableRequest") &&
                !metadataJson["lastRetryableRequest"].IsNull()) {
                rapidjson::Document retryDoc;
                retryDoc.CopyFrom(metadataJson["lastRetryableRequest"], retryDoc.GetAllocator());
                bindText(stmt, 14, firmius::shared::toJsonString(retryDoc));
            } else {
                sqlite3_bind_null(stmt, 14);
            }
            bindText(stmt, 15, threadId);
        },
        "Failed to update thread metadata");
}

std::vector<ThreadMetadata> ThreadManager::listThreadsWithMetadata() const {
    // Single-query path. Earlier this code SELECTed thread_ids and then ran
    // a separate getMetadata() (= fresh sqlite_open + 5 PRAGMAs + schema +
    // migration check + per-row SELECT) for every thread. With a few hundred
    // threads that turns ui.snapshot.get into a multi-100ms operation. We now
    // pull everything in one query on a single connection.
    auto conn = acquireConnection(basePath_);
    Statement stmt(
        conn->db,
        "SELECT thread_id, title, host_type, host_identifier, cwd, lead_persona, "
        "       is_benchmark_run, benchmark_id, benchmark_task_id, active_plan_id, "
        "       permission_mode, created_at, last_active_at, host_options_json, "
        "       retryable_request_json "
        "FROM thread_metadata_v2 ORDER BY created_at ASC;",
        "Failed to prepare list metadata query");

    std::vector<ThreadMetadata> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        try {
            ThreadMetadata meta;
            const auto* threadIdText =
                reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
            if (!threadIdText) {
                continue;
            }
            meta.threadId = threadIdText;
            const auto* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
            const auto* hostIdentifier = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
            const auto* cwd = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
            const auto* leadPersona = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
            const auto* benchmarkId = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
            const auto* benchmarkTaskId = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
            const auto* permissionMode = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 10));
            const auto* hostOptionsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 13));
            const auto* retryableRequestJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 14));
            meta.title = title ? title : "Untitled Thread";
            meta.hostIdentifier = hostIdentifier ? hostIdentifier : "";
            meta.cwd = cwd ? cwd : "";
            meta.leadPersona = leadPersona ? leadPersona : "";
            meta.isBenchmarkRun = sqlite3_column_int(stmt.get(), 6) != 0;
            meta.benchmarkId = benchmarkId ? benchmarkId : "";
            meta.benchmarkTaskId = benchmarkTaskId ? benchmarkTaskId : "";
            meta.permissionMode = permissionMode
                                       ? threadPermissionModeFromStoredString(permissionMode)
                                       : ThreadPermissionMode::Request;
            meta.createdAt = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 11));
            meta.lastActiveAt = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 12));
            if (hostOptionsJson && *hostOptionsJson) {
                auto d = parseJson(hostOptionsJson, "thread host options");
                meta.hostOptions = hostCreationOptionsFromJsonValue(d);
            }
            if (retryableRequestJson && *retryableRequestJson) {
                auto d = parseJson(retryableRequestJson, "thread retryable request");
                if (d.IsObject()) {
                    ThreadMetadata::RetryableRequest retry;
                    retry.targetAgentId = d.HasMember("targetAgentId") && d["targetAgentId"].IsString() ? d["targetAgentId"].GetString() : "";
                    retry.turnId = d.HasMember("turnId") && d["turnId"].IsString() ? d["turnId"].GetString() : "";
                    retry.text = d.HasMember("text") && d["text"].IsString() ? d["text"].GetString() : "";
                    retry.recordedAt = d.HasMember("recordedAt") && d["recordedAt"].IsUint64() ? d["recordedAt"].GetUint64() : 0;
                    retry.eligible = d.HasMember("eligible") && d["eligible"].IsBool() ? d["eligible"].GetBool() : false;
                    if (d.HasMember("images") && d["images"].IsArray()) {
                        for (const auto& imageValue : d["images"].GetArray()) {
                            try {
                                auto part = messagePartFromJsonValue(imageValue);
                                if (auto* image = std::get_if<ImageContent>(&part)) {
                                    retry.images.push_back(*image);
                                }
                            } catch (...) {
                            }
                        }
                    }
                    if (!retry.text.empty() || !retry.images.empty()) {
                        meta.lastRetryableRequest = std::move(retry);
                    }
                }
            }
            result.push_back(std::move(meta));
        } catch (...) {
            // Skip malformed metadata rows.
        }
    }
    return result;
}

AgentTodoList ThreadManager::getAgentTodo(const std::string& threadId,
                                          const std::string& agentId) const {
    if (agentId.empty()) {
        throw std::runtime_error("Cannot load todo list with empty agentId");
    }

    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT next_id FROM agent_todos_v2 WHERE thread_id=? AND agent_id=?;",
                   "Failed to prepare get todo query");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, agentId);

    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        AgentTodoList empty;
        empty.threadId = threadId;
        empty.agentId = agentId;
        empty.nextId = 1;
        return empty;
    }

    AgentTodoList list;
    list.threadId = threadId;
    list.agentId = agentId;
    list.nextId = sqlite3_column_int(stmt.get(), 0);
    if (list.nextId <= 0) {
        list.nextId = 1;
    }

    Statement itemStmt(conn->db,
                       "SELECT item_id, text, status, chunk_id, plan_id, created_at, updated_at FROM todo_items_v2 WHERE thread_id=? AND agent_id=? ORDER BY item_id ASC;",
                       "Failed to prepare get todo items query");
    bindText(itemStmt.get(), 1, threadId);
    bindText(itemStmt.get(), 2, agentId);
    while (sqlite3_step(itemStmt.get()) == SQLITE_ROW) {
        TodoItem item;
        item.id = sqlite3_column_int(itemStmt.get(), 0);
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(itemStmt.get(), 1));
        const auto* status = reinterpret_cast<const char*>(sqlite3_column_text(itemStmt.get(), 2));
        const auto* chunkId = reinterpret_cast<const char*>(sqlite3_column_text(itemStmt.get(), 3));
        const auto* planId = reinterpret_cast<const char*>(sqlite3_column_text(itemStmt.get(), 4));
        item.text = text ? text : "";
        item.chunkId = chunkId ? chunkId : "";
        item.planId = planId ? planId : "";
        item.createdAt = static_cast<uint64_t>(sqlite3_column_int64(itemStmt.get(), 5));
        item.updatedAt = static_cast<uint64_t>(sqlite3_column_int64(itemStmt.get(), 6));
        if (status) {
            if (std::string(status) == "Done") item.status = TodoStatus::Done;
            else if (std::string(status) == "InProgress") item.status = TodoStatus::InProgress;
            else item.status = TodoStatus::Pending;
        }
        list.items.push_back(std::move(item));
    }
    return list;
}

void ThreadManager::writeAgentTodo(const std::string& threadId,
                                   const std::string& agentId,
                                   const AgentTodoList& todoList) {
    if (agentId.empty()) {
        throw std::runtime_error("Cannot write todo list with empty agentId");
    }

    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);

    AgentTodoList persisted = todoList;
    if (persisted.threadId.empty()) {
        persisted.threadId = threadId;
    }
    if (persisted.agentId.empty()) {
        persisted.agentId = agentId;
    }
    if (persisted.threadId != threadId || persisted.agentId != agentId) {
        throw std::runtime_error("Todo identity does not match target thread/agent");
    }
    if (persisted.nextId <= 0) {
        persisted.nextId = 1;
    }

    withImmediateTransaction(conn->db, [&]() {
        execPrepared(
            conn->db,
            "INSERT INTO agent_todos_v2(thread_id, agent_id, next_id) VALUES(?, ?, ?) ON CONFLICT(thread_id, agent_id) DO UPDATE SET next_id=excluded.next_id;",
            [&](sqlite3_stmt* stmt) {
                bindText(stmt, 1, threadId);
                bindText(stmt, 2, agentId);
                sqlite3_bind_int(stmt, 3, persisted.nextId);
            },
            "Failed to write agent todo list");
        deleteTodoItemsV2(conn->db, threadId, agentId);
        persistTodoItemsV2(conn->db, persisted);
    });
}

AgentTodoList ThreadManager::mutateAgentTodo(
    const std::string& threadId, const std::string& agentId,
    const std::function<void(AgentTodoList&)>& mutator) {
    if (agentId.empty()) {
        throw std::runtime_error("Cannot mutate todo list with empty agentId");
    }

    auto todoMutex = acquireTodoMutex(threadId, agentId);
    std::lock_guard<std::mutex> lock(*todoMutex);
    AgentTodoList todoList = getAgentTodo(threadId, agentId);
    mutator(todoList);
    if (todoList.threadId.empty()) {
        todoList.threadId = threadId;
    }
    if (todoList.agentId.empty()) {
        todoList.agentId = agentId;
    }
    if (todoList.nextId <= 0) {
        todoList.nextId = 1;
    }
    if (todoList.threadId != threadId || todoList.agentId != agentId) {
        throw std::runtime_error("Todo identity does not match target thread/agent");
    }
    writeAgentTodo(threadId, agentId, todoList);
    return todoList;
}

AgentLiveState ThreadManager::getAgentLiveState(const std::string& threadId,
                                                const std::string& agentId) const {
    if (agentId.empty()) {
        throw std::runtime_error("Cannot load live state with empty agentId");
    }

    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT state_json FROM agent_live_state WHERE thread_id=? AND agent_id=?;",
                   "Failed to prepare get live state query");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, agentId);

    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        AgentLiveState empty;
        empty.threadId = threadId;
        empty.agentId = agentId;
        return empty;
    }

    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    auto d = parseJson(text ? text : "{}", "agent live state");
    AgentLiveState state = liveStateFromJson(d);
    if (state.threadId.empty()) {
        state.threadId = threadId;
    }
    if (state.agentId.empty()) {
        state.agentId = agentId;
    }
    return state;
}

void ThreadManager::writeAgentLiveState(const std::string& threadId,
                                        const std::string& agentId,
                                        const AgentLiveState& liveState) {
    if (agentId.empty()) {
        throw std::runtime_error("Cannot write live state with empty agentId");
    }

    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);

    AgentLiveState persisted = liveState;
    if (persisted.threadId.empty()) {
        persisted.threadId = threadId;
    }
    if (persisted.agentId.empty()) {
        persisted.agentId = agentId;
    }

    Statement stmt(conn->db,
                   "INSERT INTO agent_live_state(thread_id, agent_id, state_json) VALUES(?, ?, ?) "
                   "ON CONFLICT(thread_id, agent_id) DO UPDATE SET state_json=excluded.state_json;",
                   "Failed to prepare write live state statement");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, agentId);
    bindText(stmt.get(), 3, firmius::shared::toJsonString(liveStateToJson(persisted)));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throwSqliteError(conn->db, "Failed to write live state");
    }
}

AgentLiveState ThreadManager::mutateAgentLiveState(
    const std::string& threadId, const std::string& agentId,
    const std::function<void(AgentLiveState&)>& mutator) {
    if (agentId.empty()) {
        throw std::runtime_error("Cannot mutate live state with empty agentId");
    }

    AgentLiveState state = getAgentLiveState(threadId, agentId);
    mutator(state);
    if (state.threadId.empty()) {
        state.threadId = threadId;
    }
    if (state.agentId.empty()) {
        state.agentId = agentId;
    }
    writeAgentLiveState(threadId, agentId, state);
    return state;
}



FleetState ThreadManager::getFleetState(const std::string& threadId) const {
    auto conn = acquireConnection(basePath_);
    try {
        auto value = readThreadStateField(conn->db, threadId, "fleet_state_json");
        if (!value.has_value()) {
            return {};
        }
        auto d = parseJson(*value, "fleet state");
        FleetState state;
        if (d.IsObject() && d.HasMember("locks") && d["locks"].IsArray()) {
            for (const auto& entry : d["locks"].GetArray()) {
                FleetLock lock;
                if (entry.HasMember("lock_id") && entry["lock_id"].IsString()) {
                    lock.lockId = entry["lock_id"].GetString();
                }
                if (entry.HasMember("thread_id") && entry["thread_id"].IsString()) {
                    lock.threadId = entry["thread_id"].GetString();
                }
                if (entry.HasMember("root_agent_id") && entry["root_agent_id"].IsString()) {
                    lock.rootAgentId = entry["root_agent_id"].GetString();
                }
                if (entry.HasMember("owner_agent_id") && entry["owner_agent_id"].IsString()) {
                    lock.ownerAgentId = entry["owner_agent_id"].GetString();
                }
                if (entry.HasMember("status") && entry["status"].IsString()) {
                    lock.status = entry["status"].GetString();
                }
                if (entry.HasMember("reason") && entry["reason"].IsString()) {
                    lock.reason = entry["reason"].GetString();
                }
                if (entry.HasMember("created_at") && entry["created_at"].IsUint64()) {
                    lock.createdAt = entry["created_at"].GetUint64();
                }
                if (entry.HasMember("updated_at") && entry["updated_at"].IsUint64()) {
                    lock.updatedAt = entry["updated_at"].GetUint64();
                }
                if (entry.HasMember("paths") && entry["paths"].IsArray()) {
                    for (const auto& p : entry["paths"].GetArray()) {
                        if (p.IsString()) {
                            lock.paths.emplace_back(p.GetString());
                        }
                    }
                }
                if (entry.HasMember("waiters") && entry["waiters"].IsArray()) {
                    for (const auto& w : entry["waiters"].GetArray()) {
                        if (w.IsString()) {
                            lock.waiters.emplace_back(w.GetString());
                        }
                    }
                }
                state.locks.push_back(std::move(lock));
            }
        }
        return state;
    } catch (...) {
        return {};
    }
}

void ThreadManager::writeFleetState(const std::string& threadId,
                                    const FleetState& state) {
    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);

    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    rapidjson::Value locks(rapidjson::kArrayType);
    for (const auto& lock : state.locks) {
        rapidjson::Value obj(rapidjson::kObjectType);
        obj.AddMember("lock_id", rapidjson::Value(lock.lockId.c_str(), a), a);
        obj.AddMember("thread_id", rapidjson::Value(lock.threadId.c_str(), a), a);
        obj.AddMember("root_agent_id", rapidjson::Value(lock.rootAgentId.c_str(), a), a);
        obj.AddMember("owner_agent_id", rapidjson::Value(lock.ownerAgentId.c_str(), a), a);
        obj.AddMember("status", rapidjson::Value(lock.status.c_str(), a), a);
        obj.AddMember("reason", rapidjson::Value(lock.reason.c_str(), a), a);
        obj.AddMember("created_at", static_cast<uint64_t>(lock.createdAt), a);
        obj.AddMember("updated_at", static_cast<uint64_t>(lock.updatedAt), a);

        rapidjson::Value paths(rapidjson::kArrayType);
        for (const auto& p : lock.paths) {
            paths.PushBack(rapidjson::Value(p.c_str(), a), a);
        }
        obj.AddMember("paths", paths, a);

        rapidjson::Value waiters(rapidjson::kArrayType);
        for (const auto& w : lock.waiters) {
            waiters.PushBack(rapidjson::Value(w.c_str(), a), a);
        }
        obj.AddMember("waiters", waiters, a);
        locks.PushBack(obj, a);
    }
    d.AddMember("locks", locks, a);

    writeThreadStateField(conn->db, threadId, "fleet_state_json",
                          firmius::shared::toJsonString(d));
}

FleetState ThreadManager::mutateFleetState(
    const std::string& threadId,
    const std::function<void(FleetState&)>& mutator) {
    FleetState state = getFleetState(threadId);
    mutator(state);
    writeFleetState(threadId, state);
    return state;
}

std::vector<CompactionSnapshot>
ThreadManager::loadCompactionSnapshots(const std::string& threadId,
                                       const std::string& agentId) const {
    if (threadId.empty() || agentId.empty()) {
        return {};
    }

    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT snapshot_json FROM compaction_snapshots "
                   "WHERE thread_id=? AND agent_id=? ORDER BY id ASC;",
                   "Failed to prepare compaction snapshot read");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, agentId);

    std::vector<CompactionSnapshot> snapshots;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        if (!text) {
            continue;
        }
        auto d = parseJson(text, "compaction snapshot");
        snapshots.push_back(compactionSnapshotFromJsonValue(d));
    }
    return snapshots;
}

void ThreadManager::appendCompactionSnapshot(
    const std::string& threadId, const std::string& agentId,
    const CompactionSnapshot& snapshot) {
    auto conn = acquireConnection(basePath_);

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    auto value = compactionSnapshotToJson(snapshot, alloc);
    doc.CopyFrom(value, alloc);

    Statement stmt(conn->db,
                   "INSERT INTO compaction_snapshots(thread_id, agent_id, compaction_id, snapshot_json) "
                   "VALUES(?, ?, ?, ?);",
                   "Failed to prepare compaction snapshot write");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, agentId);
    bindText(stmt.get(), 3, snapshot.compactionId);
    bindText(stmt.get(), 4, firmius::shared::toJsonString(doc));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throwSqliteError(conn->db, "Failed to append compaction snapshot");
    }
}

bool ThreadManager::popCompactionSnapshot(
    const std::string& threadId, const std::string& agentId,
    const std::optional<std::string>& compactionId, CompactionSnapshot* removed) {
    auto conn = acquireConnection(basePath_);

    Statement query(conn->db,
                    "SELECT id, snapshot_json, compaction_id FROM compaction_snapshots "
                    "WHERE thread_id=? AND agent_id=? ORDER BY id ASC;",
                    "Failed to prepare compaction snapshot pop query");
    bindText(query.get(), 1, threadId);
    bindText(query.get(), 2, agentId);

    struct Row {
        sqlite3_int64 id = 0;
        std::string json;
        std::string compactionId;
    };
    std::vector<Row> rows;
    while (sqlite3_step(query.get()) == SQLITE_ROW) {
        const auto* jsonText =
            reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1));
        const auto* compactionIdText =
            reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 2));
        rows.push_back(Row{sqlite3_column_int64(query.get(), 0),
                           jsonText ? jsonText : "",
                           compactionIdText ? compactionIdText : ""});
    }
    if (rows.empty()) {
        return false;
    }

    auto it = rows.end() - 1;
    if (compactionId.has_value()) {
        auto reverseIt = std::find_if(rows.rbegin(), rows.rend(), [&](const Row& row) {
            return row.compactionId == *compactionId;
        });
        if (reverseIt == rows.rend()) {
            return false;
        }
        it = std::prev(reverseIt.base());
    }

    if (removed) {
        auto d = parseJson(it->json, "compaction snapshot");
        *removed = compactionSnapshotFromJsonValue(d);
    }

    Statement del(conn->db,
                  "DELETE FROM compaction_snapshots WHERE id=?;",
                  "Failed to prepare compaction snapshot delete");
    sqlite3_bind_int64(del.get(), 1, it->id);
    if (sqlite3_step(del.get()) != SQLITE_DONE) {
        throwSqliteError(conn->db, "Failed to delete compaction snapshot");
    }
    return true;
}

std::map<std::string, AgentManifestEntry>
ThreadManager::readAgentManifest(const std::string& threadId) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT agent_id, persona, parent_id, friendly_name, title, persist_history FROM agent_manifest_v2 WHERE thread_id=? ORDER BY agent_id ASC;",
                   "Failed to prepare agent manifest read");
    bindText(stmt.get(), 1, threadId);
    std::map<std::string, AgentManifestEntry> manifest;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        AgentManifestEntry entry;
        const auto* agentId = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        const auto* persona = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        const auto* parentId = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        const auto* friendlyName = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        const auto* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        entry.persona = persona ? persona : "";
        entry.parentId = parentId ? parentId : "";
        entry.friendlyName = friendlyName ? friendlyName : "";
        entry.title = title ? title : "";
        entry.persistHistory = sqlite3_column_int(stmt.get(), 5) != 0;
        if (agentId) manifest[agentId] = entry;
    }
    return manifest;
}

bool ThreadManager::tryReadAgentManifest(
    const std::string& threadId,
    std::map<std::string, AgentManifestEntry>& manifest,
    std::string* error) const {
    try {
        manifest = readAgentManifest(threadId);
        return true;
    } catch (const std::exception& ex) {
        manifest.clear();
        if (error) {
            *error = ex.what();
        }
    } catch (...) {
        manifest.clear();
        if (error) {
            *error = "Unknown error loading thread manifest";
        }
    }
    return false;
}

void ThreadManager::writeAgentManifest(
    const std::string& threadId,
    const std::map<std::string, AgentManifestEntry>& manifest) {
    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);
    withImmediateTransaction(conn->db, [&]() {
        execPrepared(conn->db,
                     "DELETE FROM agent_manifest_v2 WHERE thread_id=?;",
                     [&](sqlite3_stmt* stmt) { bindText(stmt, 1, threadId); },
                     "Failed to clear agent manifest");
        for (const auto& [agentId, entry] : manifest) {
            execPrepared(
                conn->db,
                "INSERT INTO agent_manifest_v2(thread_id, agent_id, persona, parent_id, friendly_name, title, persist_history) VALUES(?, ?, ?, ?, ?, ?, ?);",
                [&](sqlite3_stmt* stmt) {
                    bindText(stmt, 1, threadId);
                    bindText(stmt, 2, agentId);
                    bindText(stmt, 3, entry.persona);
                    bindText(stmt, 4, entry.parentId);
                    bindText(stmt, 5, entry.friendlyName);
                    bindText(stmt, 6, entry.title);
                    sqlite3_bind_int(stmt, 7, entry.persistHistory ? 1 : 0);
                },
                "Failed to write agent manifest entry");
        }
    });
}

shared::ThreadArtifactMetadata ThreadManager::writeArtifact(
    const std::string& threadId, const std::string& ownerAgentId,
    const std::string& ownerFriendlyName, const std::string& filename,
    const std::string& content, bool* created,
    const std::optional<std::string>& kind,
    const std::optional<std::string>& description) {
    if (threadId.empty()) {
        throw std::runtime_error("Cannot write artifact with empty threadId");
    }
    if (ownerAgentId.empty()) {
        throw std::runtime_error("Cannot write artifact with empty ownerAgentId");
    }
    validateArtifactFilename(filename);

    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);

    const uint64_t timestamp = nowEpochMs();
    const std::string storagePath = "artifacts/" + ownerAgentId + "/" + filename;

    Statement existingStmt(
        conn->db,
        "SELECT created_at FROM artifacts_v2 WHERE thread_id=? AND owner_agent_id=? AND filename=?;",
        "Failed to prepare artifact existence query");
    bindText(existingStmt.get(), 1, threadId);
    bindText(existingStmt.get(), 2, ownerAgentId);
    bindText(existingStmt.get(), 3, filename);

    uint64_t createdAt = timestamp;
    bool wasCreated = true;
    if (sqlite3_step(existingStmt.get()) == SQLITE_ROW) {
        wasCreated = false;
        createdAt = static_cast<uint64_t>(sqlite3_column_int64(existingStmt.get(), 0));
    }

    execPrepared(
        conn->db,
        "INSERT INTO artifacts_v2(thread_id, owner_agent_id, filename, owner_friendly_name, storage_path, content, kind, description, created_at, updated_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(thread_id, owner_agent_id, filename) DO UPDATE SET owner_friendly_name=excluded.owner_friendly_name, storage_path=excluded.storage_path, content=excluded.content, kind=COALESCE(excluded.kind, artifacts_v2.kind), description=COALESCE(excluded.description, artifacts_v2.description), updated_at=excluded.updated_at;",
        [&](sqlite3_stmt* stmt) {
            bindText(stmt, 1, threadId);
            bindText(stmt, 2, ownerAgentId);
            bindText(stmt, 3, filename);
            bindText(stmt, 4, ownerFriendlyName);
            bindText(stmt, 5, storagePath);
            bindText(stmt, 6, content);
            bindOptionalText(stmt, 7, kind);
            bindOptionalText(stmt, 8, description);
            sqlite3_bind_int64(stmt, 9, static_cast<sqlite3_int64>(createdAt));
            sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(timestamp));
        },
        "Failed to write artifact");

    if (created) {
        *created = wasCreated;
    }

    shared::ThreadArtifactMetadata metadata;
    metadata.threadId = threadId;
    metadata.ownerAgentId = ownerAgentId;
    metadata.ownerFriendlyName = ownerFriendlyName;
    metadata.filename = filename;
    metadata.storagePath = storagePath;
    metadata.createdAt = createdAt;
    metadata.updatedAt = timestamp;
    metadata.kind = kind;
    metadata.description = description;

    if (!wasCreated) {
        auto all = listArtifactsForAgent(threadId, ownerAgentId);
        for (const auto& item : all) {
            if (item.filename == filename) {
                metadata.kind = item.kind;
                metadata.description = item.description;
                metadata.createdAt = item.createdAt;
            }
        }
    }

    return metadata;
}

std::string ThreadManager::readArtifact(const std::string& threadId,
                                        const std::string& ownerAgentId,
                                        const std::string& filename) const {
    if (threadId.empty() || ownerAgentId.empty() || filename.empty()) {
        throw std::runtime_error("Artifact selector is incomplete");
    }
    validateArtifactFilename(filename);

    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT content FROM artifacts_v2 WHERE thread_id=? AND owner_agent_id=? AND filename=?;",
                   "Failed to prepare artifact read");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, ownerAgentId);
    bindText(stmt.get(), 3, filename);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error("Artifact not found: " + ownerAgentId + "/" + filename);
    }

    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    return text ? text : "";
}

std::vector<shared::ThreadArtifactMetadata>
ThreadManager::listArtifacts(const std::string& threadId) const {
    if (threadId.empty()) {
        return {};
    }

    auto conn = acquireConnection(basePath_);
    Statement stmt(
        conn->db,
        "SELECT owner_agent_id, owner_friendly_name, filename, storage_path, kind, description, created_at, updated_at FROM artifacts_v2 WHERE thread_id=? ORDER BY owner_friendly_name ASC, filename ASC;",
        "Failed to prepare artifact list");
    bindText(stmt.get(), 1, threadId);

    std::vector<shared::ThreadArtifactMetadata> artifacts;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        shared::ThreadArtifactMetadata m;
        m.threadId = threadId;
        const auto* ownerAgent = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        const auto* ownerFriendly = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        const auto* filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        const auto* storagePath = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        const auto* kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        const auto* description = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        m.ownerAgentId = ownerAgent ? ownerAgent : "";
        m.ownerFriendlyName = ownerFriendly ? ownerFriendly : "";
        m.filename = filename ? filename : "";
        m.storagePath = storagePath ? storagePath : "";
        if (kind) m.kind = std::string(kind);
        if (description) m.description = std::string(description);
        m.createdAt = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 6));
        m.updatedAt = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 7));
        artifacts.push_back(std::move(m));
    }
    return artifacts;
}

std::vector<shared::ThreadArtifactMetadata>
ThreadManager::listArtifactsForAgent(const std::string& threadId,
                                     const std::string& ownerAgentId) const {
    if (ownerAgentId.empty()) {
        return {};
    }

    auto conn = acquireConnection(basePath_);
    Statement stmt(
        conn->db,
        "SELECT owner_agent_id, owner_friendly_name, filename, storage_path, kind, description, created_at, updated_at FROM artifacts_v2 WHERE thread_id=? AND owner_agent_id=? ORDER BY filename ASC;",
        "Failed to prepare artifact list for agent");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, ownerAgentId);

    std::vector<shared::ThreadArtifactMetadata> artifacts;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        shared::ThreadArtifactMetadata m;
        m.threadId = threadId;
        const auto* ownerAgent = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        const auto* ownerFriendly = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        const auto* filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        const auto* storagePath = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        const auto* kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        const auto* description = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        m.ownerAgentId = ownerAgent ? ownerAgent : "";
        m.ownerFriendlyName = ownerFriendly ? ownerFriendly : "";
        m.filename = filename ? filename : "";
        m.storagePath = storagePath ? storagePath : "";
        if (kind) m.kind = std::string(kind);
        if (description) m.description = std::string(description);
        m.createdAt = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 6));
        m.updatedAt = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 7));
        artifacts.push_back(std::move(m));
    }
    return artifacts;
}


void ThreadManager::writeEditBatch(
    const std::string& threadId, const shared::EditBatchSummary& summary,
    const std::vector<shared::EditFileMutation>& files) {
    if (threadId.empty() || summary.editBatchId.empty()) {
        throw std::runtime_error("Edit batch persistence requires threadId and editBatchId");
    }
    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);

    withImmediateTransaction(conn->db, [&]() {
        execPrepared(
            conn->db,
            "INSERT INTO edit_batches_v1(thread_id, edit_batch_id, agent_id, parent_agent_id, friendly_name, turn_id, tool_call_id, tool_name, request_mode, created_at, status, summary_json, undo_action_batch_id) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(thread_id, edit_batch_id) DO UPDATE SET agent_id=excluded.agent_id, parent_agent_id=excluded.parent_agent_id, friendly_name=excluded.friendly_name, turn_id=excluded.turn_id, tool_call_id=excluded.tool_call_id, tool_name=excluded.tool_name, request_mode=excluded.request_mode, created_at=excluded.created_at, status=excluded.status, summary_json=excluded.summary_json, undo_action_batch_id=excluded.undo_action_batch_id;",
            [&](sqlite3_stmt* stmt) {
                bindText(stmt, 1, threadId);
                bindText(stmt, 2, summary.editBatchId);
                bindText(stmt, 3, summary.agentId);
                bindText(stmt, 4, summary.parentAgentId);
                bindText(stmt, 5, summary.friendlyName);
                bindText(stmt, 6, summary.turnId);
                bindText(stmt, 7, summary.toolCallId);
                bindText(stmt, 8, summary.toolName);
                bindText(stmt, 9, summary.requestMode);
                sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(summary.createdAt));
                bindText(stmt, 11, shared::editBatchStatusToString(summary.status));
                bindText(stmt, 12, firmius::shared::toJsonString(shared::toJson(summary)));
                bindOptionalText(stmt, 13, summary.undoActionBatchId);
            },
            "Failed to write edit batch");

        execPrepared(conn->db,
                     "DELETE FROM edit_file_mutations_v1 WHERE thread_id=? AND edit_batch_id=?;",
                     [&](sqlite3_stmt* stmt) {
                         bindText(stmt, 1, threadId);
                         bindText(stmt, 2, summary.editBatchId);
                     },
                     "Failed to clear edit file mutations");

        for (const auto& file : files) {
            execPrepared(
                conn->db,
                "INSERT INTO edit_file_mutations_v1(thread_id, file_mutation_id, edit_batch_id, file_path, ordinal_in_batch, status, mutation_json) VALUES(?, ?, ?, ?, ?, ?, ?);",
                [&](sqlite3_stmt* stmt) {
                    bindText(stmt, 1, threadId);
                    bindText(stmt, 2, file.fileMutationId);
                    bindText(stmt, 3, summary.editBatchId);
                    bindText(stmt, 4, file.filePath);
                    sqlite3_bind_int(stmt, 5, file.ordinalInBatch);
                    bindText(stmt, 6, shared::editFileMutationStatusToString(file.status));
                    bindText(stmt, 7, firmius::shared::toJsonString(shared::toJson(file)));
                },
                "Failed to write edit file mutation");
        }
    });
}

shared::EditBatchDetail ThreadManager::getEditBatch(const std::string& threadId,
                                                    const std::string& editBatchId) const {
    auto conn = acquireConnection(basePath_);
    Statement batchStmt(conn->db,
                        "SELECT summary_json FROM edit_batches_v1 WHERE thread_id=? AND edit_batch_id=?;",
                        "Failed to prepare edit batch read");
    bindText(batchStmt.get(), 1, threadId);
    bindText(batchStmt.get(), 2, editBatchId);
    if (sqlite3_step(batchStmt.get()) != SQLITE_ROW) {
        throw std::runtime_error("Edit batch not found: " + editBatchId);
    }
    const auto* summaryText = reinterpret_cast<const char*>(sqlite3_column_text(batchStmt.get(), 0));
    auto summaryDoc = parseJson(summaryText ? summaryText : "{}", "edit batch summary");
    shared::EditBatchDetail detail;
    detail.summary = shared::editBatchSummaryFromJson(summaryDoc);

    Statement fileStmt(conn->db,
                       "SELECT mutation_json FROM edit_file_mutations_v1 WHERE thread_id=? AND edit_batch_id=? ORDER BY ordinal_in_batch ASC;",
                       "Failed to prepare edit file mutations read");
    bindText(fileStmt.get(), 1, threadId);
    bindText(fileStmt.get(), 2, editBatchId);
    while (sqlite3_step(fileStmt.get()) == SQLITE_ROW) {
        const auto* mutationText = reinterpret_cast<const char*>(sqlite3_column_text(fileStmt.get(), 0));
        auto mutationDoc = parseJson(mutationText ? mutationText : "{}", "edit file mutation");
        detail.files.push_back(shared::editFileMutationFromJson(mutationDoc));
    }
    return detail;
}

std::vector<shared::EditBatchSummary>
ThreadManager::listEditBatches(const std::string& threadId,
                               const shared::EditHistoryFilters& filters) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT summary_json FROM edit_batches_v1 WHERE thread_id=? ORDER BY created_at DESC, edit_batch_id DESC;",
                   "Failed to prepare edit batch list");
    bindText(stmt.get(), 1, threadId);
    std::vector<shared::EditBatchSummary> summaries;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* summaryText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        auto doc = parseJson(summaryText ? summaryText : "{}", "edit batch list row");
        auto summary = shared::editBatchSummaryFromJson(doc);
        if (!filters.includeUndone && summary.status == shared::EditBatchStatus::Undone) {
            continue;
        }
        if (filters.agentId.has_value() && summary.agentId != *filters.agentId) {
            continue;
        }
        if (filters.parentAgentId.has_value() && summary.parentAgentId != *filters.parentAgentId) {
            continue;
        }
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

std::vector<shared::EditFileMutation>
ThreadManager::listEditFileMutationsForFile(const std::string& threadId,
                                            const std::string& filePath) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT mutation_json FROM edit_file_mutations_v1 WHERE thread_id=? AND file_path=? ORDER BY rowid DESC;",
                   "Failed to prepare edit file mutation list for file");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, filePath);
    std::vector<shared::EditFileMutation> mutations;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        auto doc = parseJson(text ? text : "{}", "edit file mutation list row");
        mutations.push_back(shared::editFileMutationFromJson(doc));
    }
    return mutations;
}

void ThreadManager::updateEditBatchStatus(
    const std::string& threadId, const std::string& editBatchId,
    shared::EditBatchStatus status,
    const std::optional<std::string>& undoActionBatchId) {
    auto detail = getEditBatch(threadId, editBatchId);
    detail.summary.status = status;
    detail.summary.undoActionBatchId = undoActionBatchId;
    writeEditBatch(threadId, detail.summary, detail.files);
}

void ThreadManager::updateEditFileMutationStatus(
    const std::string& threadId, const std::string& fileMutationId,
    shared::EditFileMutationStatus status) {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT mutation_json FROM edit_file_mutations_v1 WHERE thread_id=? AND file_mutation_id=?;",
                   "Failed to prepare edit file mutation status read");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, fileMutationId);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error("Edit file mutation not found: " + fileMutationId);
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    auto doc = parseJson(text ? text : "{}", "edit file mutation status");
    auto mutation = shared::editFileMutationFromJson(doc);
    mutation.status = status;
    execPrepared(conn->db,
                 "UPDATE edit_file_mutations_v1 SET status=?, mutation_json=? WHERE thread_id=? AND file_mutation_id=?;",
                 [&](sqlite3_stmt* updateStmt) {
                     bindText(updateStmt, 1, shared::editFileMutationStatusToString(status));
                     bindText(updateStmt, 2, firmius::shared::toJsonString(shared::toJson(mutation)));
                     bindText(updateStmt, 3, threadId);
                     bindText(updateStmt, 4, fileMutationId);
                 },
                 "Failed to update edit file mutation status");
}

void ThreadManager::writeEditUndoAction(const std::string& threadId,
                                        const shared::EditUndoAction& action) {
    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);
    execPrepared(conn->db,
                 "INSERT INTO edit_undo_actions_v1(thread_id, undo_action_id, requested_by_agent_id, target_edit_batch_id, created_at, result_status, result_json) VALUES(?, ?, ?, ?, ?, ?, ?) "
                 "ON CONFLICT(thread_id, undo_action_id) DO UPDATE SET requested_by_agent_id=excluded.requested_by_agent_id, target_edit_batch_id=excluded.target_edit_batch_id, created_at=excluded.created_at, result_status=excluded.result_status, result_json=excluded.result_json;",
                 [&](sqlite3_stmt* stmt) {
                     bindText(stmt, 1, threadId);
                     bindText(stmt, 2, action.undoActionId);
                     bindText(stmt, 3, action.requestedByAgentId);
                     bindText(stmt, 4, action.targetEditBatchId);
                     sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(action.createdAt));
                     bindText(stmt, 6, shared::editUndoResultStatusToString(action.resultStatus));
                     bindText(stmt, 7, action.resultJson);
                 },
                 "Failed to write edit undo action");
}

std::optional<shared::EditUndoAction>
ThreadManager::findEditUndoAction(const std::string& threadId,
                                  const std::string& undoActionId) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT requested_by_agent_id, target_edit_batch_id, created_at, result_status, result_json FROM edit_undo_actions_v1 WHERE thread_id=? AND undo_action_id=?;",
                   "Failed to prepare edit undo action read");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, undoActionId);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    shared::EditUndoAction action;
    action.threadId = threadId;
    action.undoActionId = undoActionId;
    const auto* requestedBy = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    const auto* targetBatch = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    const auto* resultStatus = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    const auto* resultJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
    action.requestedByAgentId = requestedBy ? requestedBy : "";
    action.targetEditBatchId = targetBatch ? targetBatch : "";
    action.createdAt = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 2));
    action.resultStatus = shared::stringToEditUndoResultStatus(resultStatus ? resultStatus : "Succeeded");
    action.resultJson = resultJson ? resultJson : "";
    return action;
}

void ThreadManager::writeEditRedoAction(const std::string& threadId,
                                        const shared::EditRedoAction& action) {
    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);
    execPrepared(conn->db,
                 "INSERT INTO edit_redo_actions_v1(thread_id, redo_action_id, target_undo_action_id, created_at, result_json) VALUES(?, ?, ?, ?, ?) "
                 "ON CONFLICT(thread_id, redo_action_id) DO UPDATE SET target_undo_action_id=excluded.target_undo_action_id, created_at=excluded.created_at, result_json=excluded.result_json;",
                 [&](sqlite3_stmt* stmt) {
                     bindText(stmt, 1, threadId);
                     bindText(stmt, 2, action.redoActionId);
                     bindText(stmt, 3, action.targetUndoActionId);
                     sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(action.createdAt));
                     bindText(stmt, 5, action.resultJson);
                 },
                 "Failed to write edit redo action");
}

std::optional<shared::EditRedoAction>
ThreadManager::findEditRedoAction(const std::string& threadId,
                                  const std::string& redoActionId) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT target_undo_action_id, created_at, result_json FROM edit_redo_actions_v1 WHERE thread_id=? AND redo_action_id=?;",
                   "Failed to prepare edit redo action read");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, redoActionId);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    shared::EditRedoAction action;
    action.threadId = threadId;
    action.redoActionId = redoActionId;
    const auto* targetUndo = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    const auto* resultJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    action.targetUndoActionId = targetUndo ? targetUndo : "";
    action.createdAt = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 1));
    action.resultJson = resultJson ? resultJson : "";
    return action;
}

void ThreadManager::writeTranscriptUndoAction(
    const std::string& threadId, const shared::TranscriptUndoAction& action,
    const std::vector<shared::TranscriptRedoPayload>& payloads) {
    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);
    withImmediateTransaction(conn->db, [&]() {
        execPrepared(conn->db,
                     "INSERT INTO transcript_undo_actions_v1(thread_id, undo_action_id, agent_id, scope_type, scope_arg_json, created_at, redo_available, reason, action_json) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
                     "ON CONFLICT(thread_id, undo_action_id) DO UPDATE SET agent_id=excluded.agent_id, scope_type=excluded.scope_type, scope_arg_json=excluded.scope_arg_json, created_at=excluded.created_at, redo_available=excluded.redo_available, reason=excluded.reason, action_json=excluded.action_json;",
                     [&](sqlite3_stmt* stmt) {
                         bindText(stmt, 1, threadId);
                         bindText(stmt, 2, action.undoActionId);
                         bindText(stmt, 3, action.agentId);
                         bindText(stmt, 4, action.scopeType);
                         bindText(stmt, 5, action.scopeArgJson);
                         sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(action.createdAt));
                         sqlite3_bind_int(stmt, 7, action.redoAvailable ? 1 : 0);
                         bindText(stmt, 8, action.reason);
                         bindText(stmt, 9, firmius::shared::toJsonString(shared::toJson(action)));
                     },
                     "Failed to write transcript undo action");
        execPrepared(conn->db,
                     "DELETE FROM transcript_redo_payloads_v1 WHERE thread_id=? AND undo_action_id=?;",
                     [&](sqlite3_stmt* stmt) {
                         bindText(stmt, 1, threadId);
                         bindText(stmt, 2, action.undoActionId);
                     },
                     "Failed to clear transcript redo payloads");
        for (const auto& payload : payloads) {
            execPrepared(conn->db,
                         "INSERT INTO transcript_redo_payloads_v1(thread_id, undo_action_id, ordinal, payload_json) VALUES(?, ?, ?, ?);",
                         [&](sqlite3_stmt* stmt) {
                             bindText(stmt, 1, threadId);
                             bindText(stmt, 2, action.undoActionId);
                             sqlite3_bind_int(stmt, 3, payload.ordinal);
                             bindText(stmt, 4, firmius::shared::toJsonString(shared::toJson(payload)));
                         },
                         "Failed to write transcript redo payload");
        }
    });
}

std::optional<shared::TranscriptUndoAction>
ThreadManager::findTranscriptUndoAction(const std::string& threadId,
                                        const std::string& undoActionId) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT action_json FROM transcript_undo_actions_v1 WHERE thread_id=? AND undo_action_id=?;",
                   "Failed to prepare transcript undo action read");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, undoActionId);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    auto doc = parseJson(text ? text : "{}", "transcript undo action");
    return shared::transcriptUndoActionFromJson(doc);
}

std::vector<shared::TranscriptUndoAction>
ThreadManager::listTranscriptUndoActions(const std::string& threadId, int limit) const {
    if (threadId.empty() || limit <= 0) {
        return {};
    }
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT action_json FROM transcript_undo_actions_v1 WHERE thread_id=? ORDER BY created_at DESC LIMIT ?;",
                   "Failed to prepare transcript undo action list");
    bindText(stmt.get(), 1, threadId);
    sqlite3_bind_int(stmt.get(), 2, limit);

    std::vector<shared::TranscriptUndoAction> actions;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        auto doc = parseJson(text ? text : "{}", "transcript undo action");
        actions.push_back(shared::transcriptUndoActionFromJson(doc));
    }
    return actions;
}

std::vector<shared::TranscriptRedoPayload>
ThreadManager::loadTranscriptRedoPayloads(const std::string& threadId,
                                          const std::string& undoActionId) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT payload_json FROM transcript_redo_payloads_v1 WHERE thread_id=? AND undo_action_id=? ORDER BY ordinal ASC;",
                   "Failed to prepare transcript redo payload read");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, undoActionId);
    std::vector<shared::TranscriptRedoPayload> payloads;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        auto doc = parseJson(text ? text : "{}", "transcript redo payload");
        payloads.push_back(shared::transcriptRedoPayloadFromJson(doc));
    }
    return payloads;
}

void ThreadManager::markTranscriptUndoRedoAvailability(
    const std::string& threadId, const std::string& undoActionId, bool available) {
    auto existing = findTranscriptUndoAction(threadId, undoActionId);
    if (!existing.has_value()) {
        return;
    }
    existing->redoAvailable = available;
    auto conn = acquireConnection(basePath_);
    execPrepared(conn->db,
                 "UPDATE transcript_undo_actions_v1 SET redo_available=?, action_json=? WHERE thread_id=? AND undo_action_id=?;",
                 [&](sqlite3_stmt* stmt) {
                     sqlite3_bind_int(stmt, 1, available ? 1 : 0);
                     bindText(stmt, 2, firmius::shared::toJsonString(shared::toJson(*existing)));
                     bindText(stmt, 3, threadId);
                     bindText(stmt, 4, undoActionId);
                 },
                 "Failed to update transcript undo redo availability");
}

std::optional<std::string>
ThreadManager::findAgentIdByFriendlyName(const std::string& threadId,
                                         const std::string& friendlyName) const {
    if (threadId.empty() || friendlyName.empty()) {
        return std::nullopt;
    }
    const auto manifest = readAgentManifest(threadId);
    std::optional<std::string> match;
    for (const auto& [agentId, entry] : manifest) {
        if (entry.friendlyName != friendlyName) {
            continue;
        }
        if (match.has_value() && *match != agentId) {
            return std::nullopt;
        }
        match = agentId;
    }
    return match;
}

std::optional<std::string>
ThreadManager::findFriendlyNameByAgentId(const std::string& threadId,
                                         const std::string& agentId) const {
    if (threadId.empty() || agentId.empty()) {
        return std::nullopt;
    }
    const auto manifest = readAgentManifest(threadId);
    auto it = manifest.find(agentId);
    if (it == manifest.end()) {
        return std::nullopt;
    }
    return it->second.friendlyName;
}

} // namespace firmius::core
