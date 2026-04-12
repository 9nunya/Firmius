#include "persistence/ThreadManager.hpp"

#include "AgentRegistry.hpp"
#include "Serialization.hpp"
#include "tools/WorkToolCommon.hpp"
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
#include <unordered_map>
#include <vector>

namespace firmius::core {

namespace {

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

std::string rapidJsonToString(const rapidjson::Document& d) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    return buffer.GetString();
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

struct SqliteConnection {
    explicit SqliteConnection(const std::string& dbPath) {
        if (sqlite3_open_v2(dbPath.c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string err = db ? sqlite3_errmsg(db) : "unknown sqlite error";
            if (db) {
                sqlite3_close(db);
                db = nullptr;
            }
            throw std::runtime_error("Failed to open thread database: " + err);
        }

        execSql(db, "PRAGMA busy_timeout=5000;",
                "Failed to set sqlite busy timeout");
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        execSql(db, "PRAGMA synchronous=NORMAL;",
                "Failed to configure sqlite synchronous mode");
        execSql(db, "PRAGMA temp_store=MEMORY;",
                "Failed to configure sqlite temp store");
        execSql(db, "PRAGMA foreign_keys=ON;",
                "Failed to enable sqlite foreign keys");

        execSql(db,
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
                "CREATE TABLE IF NOT EXISTS plans ("
                " thread_id TEXT NOT NULL,"
                " plan_id TEXT NOT NULL,"
                " plan_json TEXT NOT NULL,"
                " created_at INTEGER NOT NULL,"
                " updated_at INTEGER NOT NULL,"
                " PRIMARY KEY(thread_id, plan_id)"
                ");"
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
                "CREATE TABLE IF NOT EXISTS rolling_memory_state ("
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
                ");",
                "Failed to initialize thread database schema");
    }

    ~SqliteConnection() {
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    }

    sqlite3* db = nullptr;
};

std::shared_ptr<SqliteConnection> acquireConnection(const std::string& basePath) {
    const std::string dbPath = dbPathForBase(basePath);
    std::filesystem::create_directories(basePath);
    return std::make_shared<SqliteConnection>(dbPath);
}

void ensureThreadExists(sqlite3* db, const std::string& threadId) {
    Statement stmt(db, "SELECT 1 FROM threads WHERE thread_id=? LIMIT 1;",
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

rapidjson::Value rollingMemoryChunkToJson(const RollingMemoryChunk& chunk,
                                          rapidjson::Document::AllocatorType& a) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("chunkId", rapidjson::Value(chunk.chunkId.c_str(), a), a);
    v.AddMember("sourceStartTurnId",
                rapidjson::Value(chunk.sourceStartTurnId.c_str(), a), a);
    v.AddMember("sourceEndTurnId",
                rapidjson::Value(chunk.sourceEndTurnId.c_str(), a), a);
    rapidjson::Value ids(rapidjson::kArrayType);
    for (const auto& id : chunk.sourceTurnIds) {
        ids.PushBack(rapidjson::Value(id.c_str(), a), a);
    }
    v.AddMember("sourceTurnIds", ids, a);
    v.AddMember("summary", rapidjson::Value(chunk.summary.c_str(), a), a);
    v.AddMember("currentTask", rapidjson::Value(chunk.currentTask.c_str(), a), a);
    v.AddMember("suggestedResponse",
                rapidjson::Value(chunk.suggestedResponse.c_str(), a), a);
    v.AddMember("sourceTokens", chunk.sourceTokens, a);
    v.AddMember("summaryTokens", chunk.summaryTokens, a);
    v.AddMember("createdAt", chunk.createdAt, a);
    v.AddMember("buffered", chunk.buffered, a);
    v.AddMember("active", chunk.active, a);
    v.AddMember("superseded", chunk.superseded, a);
    return v;
}

RollingMemoryChunk rollingMemoryChunkFromJson(const rapidjson::Value& value) {
    RollingMemoryChunk chunk;
    if (!value.IsObject()) {
        return chunk;
    }
    if (value.HasMember("chunkId") && value["chunkId"].IsString()) {
        chunk.chunkId = value["chunkId"].GetString();
    }
    if (value.HasMember("sourceStartTurnId") &&
        value["sourceStartTurnId"].IsString()) {
        chunk.sourceStartTurnId = value["sourceStartTurnId"].GetString();
    }
    if (value.HasMember("sourceEndTurnId") && value["sourceEndTurnId"].IsString()) {
        chunk.sourceEndTurnId = value["sourceEndTurnId"].GetString();
    }
    if (value.HasMember("sourceTurnIds") && value["sourceTurnIds"].IsArray()) {
        for (const auto& id : value["sourceTurnIds"].GetArray()) {
            if (id.IsString()) {
                chunk.sourceTurnIds.push_back(id.GetString());
            }
        }
    }
    if (value.HasMember("summary") && value["summary"].IsString()) {
        chunk.summary = value["summary"].GetString();
    }
    if (value.HasMember("currentTask") && value["currentTask"].IsString()) {
        chunk.currentTask = value["currentTask"].GetString();
    }
    if (value.HasMember("suggestedResponse") && value["suggestedResponse"].IsString()) {
        chunk.suggestedResponse = value["suggestedResponse"].GetString();
    }
    if (value.HasMember("sourceTokens") && value["sourceTokens"].IsUint()) {
        chunk.sourceTokens = value["sourceTokens"].GetUint();
    }
    if (value.HasMember("summaryTokens") && value["summaryTokens"].IsUint()) {
        chunk.summaryTokens = value["summaryTokens"].GetUint();
    }
    if (value.HasMember("createdAt") && value["createdAt"].IsUint64()) {
        chunk.createdAt = value["createdAt"].GetUint64();
    }
    if (value.HasMember("buffered") && value["buffered"].IsBool()) {
        chunk.buffered = value["buffered"].GetBool();
    }
    if (value.HasMember("active") && value["active"].IsBool()) {
        chunk.active = value["active"].GetBool();
    }
    if (value.HasMember("superseded") && value["superseded"].IsBool()) {
        chunk.superseded = value["superseded"].GetBool();
    }
    return chunk;
}

rapidjson::Document rollingMemoryStateToJson(const RollingMemoryState& state) {
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    d.AddMember("threadId", rapidjson::Value(state.threadId.c_str(), a), a);
    d.AddMember("agentId", rapidjson::Value(state.agentId.c_str(), a), a);
    d.AddMember("lastObservedTurnId",
                rapidjson::Value(state.lastObservedTurnId.c_str(), a), a);
    d.AddMember("lastReflectedObservationId",
                rapidjson::Value(state.lastReflectedObservationId.c_str(), a), a);
    d.AddMember("lastContextWindow", state.lastContextWindow, a);
    d.AddMember("lastBufferThresholdTokens", state.lastBufferThresholdTokens, a);
    d.AddMember("lastTargetThresholdTokens", state.lastTargetThresholdTokens, a);
    d.AddMember("lastEmergencyThresholdTokens", state.lastEmergencyThresholdTokens,
                a);
    d.AddMember("lastRetainedTailTokens", state.lastRetainedTailTokens, a);
    d.AddMember("lastUpdatedAt", state.lastUpdatedAt, a);
    d.AddMember("observationInFlight", state.observationInFlight, a);
    d.AddMember("reflectionInFlight", state.reflectionInFlight, a);

    rapidjson::Value observations(rapidjson::kArrayType);
    for (const auto& chunk : state.observationChunks) {
        observations.PushBack(rollingMemoryChunkToJson(chunk, a), a);
    }
    d.AddMember("observationChunks", observations, a);

    rapidjson::Value reflections(rapidjson::kArrayType);
    for (const auto& chunk : state.reflectionChunks) {
        reflections.PushBack(rollingMemoryChunkToJson(chunk, a), a);
    }
    d.AddMember("reflectionChunks", reflections, a);
    return d;
}

RollingMemoryState rollingMemoryStateFromJson(const rapidjson::Value& value) {
    RollingMemoryState state;
    if (!value.IsObject()) {
        return state;
    }
    if (value.HasMember("threadId") && value["threadId"].IsString()) {
        state.threadId = value["threadId"].GetString();
    }
    if (value.HasMember("agentId") && value["agentId"].IsString()) {
        state.agentId = value["agentId"].GetString();
    }
    if (value.HasMember("lastObservedTurnId") &&
        value["lastObservedTurnId"].IsString()) {
        state.lastObservedTurnId = value["lastObservedTurnId"].GetString();
    }
    if (value.HasMember("lastReflectedObservationId") &&
        value["lastReflectedObservationId"].IsString()) {
        state.lastReflectedObservationId =
            value["lastReflectedObservationId"].GetString();
    }
    if (value.HasMember("lastContextWindow") && value["lastContextWindow"].IsUint()) {
        state.lastContextWindow = value["lastContextWindow"].GetUint();
    }
    if (value.HasMember("lastBufferThresholdTokens") &&
        value["lastBufferThresholdTokens"].IsUint()) {
        state.lastBufferThresholdTokens = value["lastBufferThresholdTokens"].GetUint();
    }
    if (value.HasMember("lastTargetThresholdTokens") &&
        value["lastTargetThresholdTokens"].IsUint()) {
        state.lastTargetThresholdTokens = value["lastTargetThresholdTokens"].GetUint();
    }
    if (value.HasMember("lastEmergencyThresholdTokens") &&
        value["lastEmergencyThresholdTokens"].IsUint()) {
        state.lastEmergencyThresholdTokens =
            value["lastEmergencyThresholdTokens"].GetUint();
    }
    if (value.HasMember("lastRetainedTailTokens") &&
        value["lastRetainedTailTokens"].IsUint()) {
        state.lastRetainedTailTokens = value["lastRetainedTailTokens"].GetUint();
    }
    if (value.HasMember("lastUpdatedAt") && value["lastUpdatedAt"].IsUint64()) {
        state.lastUpdatedAt = value["lastUpdatedAt"].GetUint64();
    }
    if (value.HasMember("observationInFlight") &&
        value["observationInFlight"].IsBool()) {
        state.observationInFlight = value["observationInFlight"].GetBool();
    }
    if (value.HasMember("reflectionInFlight") &&
        value["reflectionInFlight"].IsBool()) {
        state.reflectionInFlight = value["reflectionInFlight"].GetBool();
    }
    if (value.HasMember("observationChunks") && value["observationChunks"].IsArray()) {
        for (const auto& chunk : value["observationChunks"].GetArray()) {
            state.observationChunks.push_back(rollingMemoryChunkFromJson(chunk));
        }
    }
    if (value.HasMember("reflectionChunks") && value["reflectionChunks"].IsArray()) {
        for (const auto& chunk : value["reflectionChunks"].GetArray()) {
            state.reflectionChunks.push_back(rollingMemoryChunkFromJson(chunk));
        }
    }
    return state;
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

std::shared_ptr<std::mutex> acquirePlanMutex(const std::string& threadId,
                                             const std::string& planId) {
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> registry;
    const std::string key = threadId + ":" + planId;
    std::lock_guard<std::mutex> guard(registryMutex);
    if (auto existing = registry[key].lock()) {
        return existing;
    }
    auto created = std::make_shared<std::mutex>();
    registry[key] = created;
    return created;
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

std::string ThreadManager::defaultBasePath() {
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path userPath =
            std::filesystem::path(home) / ".firmius" / "threads";
        if (ensureWritableDirectory(userPath)) {
            return userPath.string();
        }
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
                   "SELECT thread_id FROM threads ORDER BY created_at ASC, thread_id ASC;",
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

    Statement stmt(conn->db,
                   "INSERT INTO threads(thread_id, metadata_json, created_at, last_active_at)"
                   " VALUES(?, ?, ?, ?);",
                   "Failed to prepare create thread statement");
    bindText(stmt.get(), 1, persisted.threadId);
    bindText(stmt.get(), 2, rapidJsonToString(toJson(persisted)));
    sqlite3_bind_int64(stmt.get(), 3, static_cast<sqlite3_int64>(persisted.createdAt));
    sqlite3_bind_int64(stmt.get(), 4, static_cast<sqlite3_int64>(persisted.lastActiveAt));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throwSqliteError(conn->db, "Failed to create thread");
    }

    return persisted.threadId;
}

ThreadMetadata ThreadManager::getMetadata(const std::string& threadId) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT metadata_json FROM threads WHERE thread_id=?;",
                   "Failed to prepare thread metadata query");
    bindText(stmt.get(), 1, threadId);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error("Thread not found: " + threadId);
    }

    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    const std::string json = text ? text : "";
    auto d = parseJson(json, "thread metadata");
    auto meta = threadMetadataFromJson(d);
    if (meta.threadId.empty()) {
        meta.threadId = threadId;
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

    Statement stmt(conn->db,
                   "SELECT turn_json FROM agent_turns "
                   "WHERE thread_id=? AND agent_id=? ORDER BY id ASC;",
                   "Failed to prepare agent history query");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, agentId);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        if (!text) {
            continue;
        }
        try {
            auto d = parseJson(text, "agent history row");
            history.turns.push_back(agentTurnFromJsonValue(d));
        } catch (...) {
            // Skip malformed rows rather than failing thread resume.
        }
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
                   "SELECT DISTINCT agent_id FROM agent_turns WHERE thread_id=? "
                   "ORDER BY agent_id ASC;",
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
            "DELETE FROM plans WHERE thread_id=?;",
            "DELETE FROM agent_todos WHERE thread_id=?;",
            "DELETE FROM agent_live_state WHERE thread_id=?;",
            "DELETE FROM rolling_memory_state WHERE thread_id=?;",
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
    Statement stmt(conn->db,
                   "UPDATE threads SET metadata_json=?, created_at=?, last_active_at=? "
                   "WHERE thread_id=?;",
                   "Failed to prepare metadata update");
    bindText(stmt.get(), 1, rapidJsonToString(toJson(persisted)));
    sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(persisted.createdAt));
    sqlite3_bind_int64(stmt.get(), 3,
                       static_cast<sqlite3_int64>(persisted.lastActiveAt));
    bindText(stmt.get(), 4, threadId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throwSqliteError(conn->db, "Failed to update thread metadata");
    }
}

std::vector<ThreadMetadata> ThreadManager::listThreadsWithMetadata() const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT thread_id, metadata_json FROM threads ORDER BY created_at ASC;",
                   "Failed to prepare list metadata query");

    std::vector<ThreadMetadata> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* threadIdText =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        const auto* metadataText =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        if (!threadIdText || !metadataText) {
            continue;
        }
        try {
            auto d = parseJson(metadataText, "thread metadata");
            auto meta = threadMetadataFromJson(d);
            if (meta.threadId.empty()) {
                meta.threadId = threadIdText;
            }
            result.push_back(std::move(meta));
        } catch (...) {
            // Skip malformed metadata rows.
        }
    }
    return result;
}

std::string ThreadManager::createPlan(const Plan& plan) {
    Plan persistedPlan = plan;
    if (persistedPlan.threadId.empty()) {
        throw std::runtime_error("Cannot create plan with empty threadId");
    }

    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, persistedPlan.threadId);

    if (persistedPlan.id.empty()) {
        persistedPlan.id = StringUtil::generateUuid();
    }
    const uint64_t timestamp = nowEpochMs();
    if (persistedPlan.createdAt == 0) {
        persistedPlan.createdAt = timestamp;
    }
    persistedPlan.updatedAt = timestamp;

    writePlan(persistedPlan.threadId, persistedPlan);
    return persistedPlan.id;
}

void ThreadManager::writePlan(const std::string& threadId, const Plan& plan) {
    if (plan.id.empty()) {
        throw std::runtime_error("Cannot write plan with empty id");
    }

    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);

    Plan persistedPlan = plan;
    if (persistedPlan.threadId.empty()) {
        persistedPlan.threadId = threadId;
    }

    Statement stmt(conn->db,
                   "INSERT INTO plans(thread_id, plan_id, plan_json, created_at, updated_at)"
                   " VALUES(?, ?, ?, ?, ?)"
                   " ON CONFLICT(thread_id, plan_id) DO UPDATE SET"
                   " plan_json=excluded.plan_json,"
                   " created_at=excluded.created_at,"
                   " updated_at=excluded.updated_at;",
                   "Failed to prepare write plan statement");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, persistedPlan.id);
    bindText(stmt.get(), 3, rapidJsonToString(toJson(persistedPlan)));
    sqlite3_bind_int64(stmt.get(), 4,
                       static_cast<sqlite3_int64>(persistedPlan.createdAt));
    sqlite3_bind_int64(stmt.get(), 5,
                       static_cast<sqlite3_int64>(persistedPlan.updatedAt));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throwSqliteError(conn->db, "Failed to write plan");
    }
}

Plan ThreadManager::getPlan(const std::string& threadId,
                            const std::string& planId) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT plan_json FROM plans WHERE thread_id=? AND plan_id=?;",
                   "Failed to prepare get plan query");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, planId);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error("Plan not found: " + planId);
    }

    const auto* jsonText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    if (!jsonText) {
        throw std::runtime_error("Plan row is empty: " + planId);
    }

    auto d = parseJson(jsonText, "plan");
    Plan plan = planFromJson(d);
    if (plan.id.empty()) {
        plan.id = planId;
    }
    if (plan.threadId.empty()) {
        plan.threadId = threadId;
    }
    worktools::reconcileChunkDependencies(plan);
    return plan;
}

std::vector<Plan> ThreadManager::listPlans(const std::string& threadId) const {
    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT plan_id FROM plans WHERE thread_id=? ORDER BY plan_id ASC;",
                   "Failed to prepare list plans query");
    bindText(stmt.get(), 1, threadId);

    std::vector<Plan> plans;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* planIdText =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        if (!planIdText) {
            continue;
        }
        plans.push_back(getPlan(threadId, planIdText));
    }
    return plans;
}

void ThreadManager::updatePlan(const std::string& threadId, const Plan& plan) {
    if (plan.id.empty()) {
        throw std::runtime_error("Cannot update plan with empty id");
    }

    auto planMutex = acquirePlanMutex(threadId, plan.id);
    std::lock_guard<std::mutex> lock(*planMutex);

    Plan persistedPlan = plan;
    if (persistedPlan.threadId.empty()) {
        persistedPlan.threadId = threadId;
    }
    if (persistedPlan.threadId != threadId) {
        throw std::runtime_error("Plan threadId does not match target thread");
    }

    const Plan existing = getPlan(threadId, plan.id);
    if (persistedPlan.createdAt == 0) {
        persistedPlan.createdAt = existing.createdAt;
    }
    persistedPlan.updatedAt = nowEpochMs();
    writePlan(threadId, persistedPlan);
}

Plan ThreadManager::mutatePlan(const std::string& threadId,
                               const std::string& planId,
                               const std::function<void(Plan&)>& mutator) {
    if (planId.empty()) {
        throw std::runtime_error("Cannot mutate plan with empty id");
    }

    auto planMutex = acquirePlanMutex(threadId, planId);
    std::lock_guard<std::mutex> lock(*planMutex);
    Plan plan = getPlan(threadId, planId);
    mutator(plan);
    if (plan.threadId.empty()) {
        plan.threadId = threadId;
    }
    if (plan.threadId != threadId) {
        throw std::runtime_error("Plan threadId does not match target thread");
    }
    if (plan.createdAt == 0) {
        plan.createdAt = nowEpochMs();
    }
    plan.updatedAt = nowEpochMs();
    writePlan(threadId, plan);
    return plan;
}

AgentTodoList ThreadManager::getAgentTodo(const std::string& threadId,
                                          const std::string& agentId) const {
    if (agentId.empty()) {
        throw std::runtime_error("Cannot load todo list with empty agentId");
    }

    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT todo_json FROM agent_todos WHERE thread_id=? AND agent_id=?;",
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

    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    auto d = parseJson(text ? text : "{}", "agent todo list");
    AgentTodoList list = agentTodoListFromJson(d);
    if (list.threadId.empty()) {
        list.threadId = threadId;
    }
    if (list.agentId.empty()) {
        list.agentId = agentId;
    }
    if (list.nextId <= 0) {
        list.nextId = 1;
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

    Statement stmt(conn->db,
                   "INSERT INTO agent_todos(thread_id, agent_id, todo_json) VALUES(?, ?, ?) "
                   "ON CONFLICT(thread_id, agent_id) DO UPDATE SET todo_json=excluded.todo_json;",
                   "Failed to prepare write todo statement");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, agentId);
    bindText(stmt.get(), 3, rapidJsonToString(toJson(persisted)));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throwSqliteError(conn->db, "Failed to write agent todo list");
    }
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
    bindText(stmt.get(), 3, rapidJsonToString(liveStateToJson(persisted)));
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

RollingMemoryState ThreadManager::loadRollingMemoryState(
    const std::string& threadId, const std::string& agentId) const {
    if (agentId.empty()) {
        throw std::runtime_error(
            "Cannot load rolling memory state with empty agentId");
    }

    auto conn = acquireConnection(basePath_);
    Statement stmt(conn->db,
                   "SELECT state_json FROM rolling_memory_state WHERE thread_id=? AND agent_id=?;",
                   "Failed to prepare get rolling memory query");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, agentId);

    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        RollingMemoryState empty;
        empty.threadId = threadId;
        empty.agentId = agentId;
        return empty;
    }

    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    auto d = parseJson(text ? text : "{}", "rolling memory state");
    RollingMemoryState state = rollingMemoryStateFromJson(d);
    if (state.threadId.empty()) {
        state.threadId = threadId;
    }
    if (state.agentId.empty()) {
        state.agentId = agentId;
    }
    return state;
}

void ThreadManager::writeRollingMemoryState(const std::string& threadId,
                                            const std::string& agentId,
                                            const RollingMemoryState& state) {
    if (agentId.empty()) {
        throw std::runtime_error(
            "Cannot write rolling memory state with empty agentId");
    }

    auto conn = acquireConnection(basePath_);
    ensureThreadExists(conn->db, threadId);

    RollingMemoryState persisted = state;
    if (persisted.threadId.empty()) {
        persisted.threadId = threadId;
    }
    if (persisted.agentId.empty()) {
        persisted.agentId = agentId;
    }
    persisted.lastUpdatedAt = nowEpochMs();

    Statement stmt(conn->db,
                   "INSERT INTO rolling_memory_state(thread_id, agent_id, state_json) VALUES(?, ?, ?) "
                   "ON CONFLICT(thread_id, agent_id) DO UPDATE SET state_json=excluded.state_json;",
                   "Failed to prepare rolling memory write");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, agentId);
    bindText(stmt.get(), 3, rapidJsonToString(rollingMemoryStateToJson(persisted)));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throwSqliteError(conn->db, "Failed to write rolling memory state");
    }
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
                          rapidJsonToString(d));
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
    bindText(stmt.get(), 4, rapidJsonToString(doc));
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
    auto json = readThreadStateField(conn->db, threadId, "agent_manifest_json");
    std::map<std::string, AgentManifestEntry> manifest;
    if (!json.has_value()) {
        return manifest;
    }

    auto d = parseJson(*json, "agent manifest");
    if (!d.IsObject()) {
        return manifest;
    }

    for (auto& m : d.GetObject()) {
        AgentManifestEntry entry;
        auto& val = m.value;

        if (val.HasMember("persona") && val["persona"].IsString()) {
            entry.persona = val["persona"].GetString();
        }
        if (val.HasMember("parentId") && val["parentId"].IsString()) {
            entry.parentId = val["parentId"].GetString();
        }
        if (val.HasMember("friendlyName") && val["friendlyName"].IsString()) {
            entry.friendlyName = val["friendlyName"].GetString();
        }
        if (val.HasMember("title") && val["title"].IsString()) {
            entry.title = val["title"].GetString();
        }
        if (val.HasMember("persistHistory") && val["persistHistory"].IsBool()) {
            entry.persistHistory = val["persistHistory"].GetBool();
        } else {
            entry.persistHistory = true;
        }

        manifest[m.name.GetString()] = entry;
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

    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();

    for (const auto& [agentId, entry] : manifest) {
        rapidjson::Value obj(rapidjson::kObjectType);
        obj.AddMember("persona", rapidjson::Value(entry.persona.c_str(), a).Move(), a);
        obj.AddMember("parentId", rapidjson::Value(entry.parentId.c_str(), a).Move(), a);
        obj.AddMember("friendlyName", rapidjson::Value(entry.friendlyName.c_str(), a).Move(), a);
        obj.AddMember("title", rapidjson::Value(entry.title.c_str(), a).Move(), a);
        obj.AddMember("persistHistory", entry.persistHistory, a);
        d.AddMember(rapidjson::Value(agentId.c_str(), a).Move(), obj, a);
    }

    writeThreadStateField(conn->db, threadId, "agent_manifest_json",
                          rapidJsonToString(d));
}

ThreadPermissionRules ThreadManager::readPermissionRules(
    const std::string& threadId) const {
    auto conn = acquireConnection(basePath_);
    auto json = readThreadStateField(conn->db, threadId, "permission_rules_json");

    ThreadPermissionRules rules;
    if (!json.has_value()) {
        return rules;
    }

    auto d = parseJson(*json, "permission rules");
    if (!d.IsObject()) {
        return rules;
    }

    if (d.HasMember("command_allow_rules") && d["command_allow_rules"].IsArray()) {
        for (const auto& ruleValue : d["command_allow_rules"].GetArray()) {
            if (!ruleValue.IsObject()) {
                continue;
            }

            CommandAllowRule rule;
            if (ruleValue.HasMember("exact_command") &&
                ruleValue["exact_command"].IsString()) {
                rule.exactCommand = ruleValue["exact_command"].GetString();
            }
            if (ruleValue.HasMember("normalized_command") &&
                ruleValue["normalized_command"].IsString()) {
                rule.normalizedCommand =
                    ruleValue["normalized_command"].GetString();
            }
            if (ruleValue.HasMember("primary_command") &&
                ruleValue["primary_command"].IsString()) {
                rule.primaryCommand = ruleValue["primary_command"].GetString();
            }
            if (ruleValue.HasMember("severity") &&
                ruleValue["severity"].IsString()) {
                rule.severity = severityFromString(ruleValue["severity"].GetString());
            }
            if (!rule.exactCommand.empty()) {
                rules.commandAllowRules.push_back(std::move(rule));
            }
        }
    }

    if (d.HasMember("write_allow_paths") && d["write_allow_paths"].IsArray()) {
        for (const auto& pathValue : d["write_allow_paths"].GetArray()) {
            if (pathValue.IsString()) {
                rules.writeAllowPaths.push_back(pathValue.GetString());
            }
        }
    }

    return rules;
}

void ThreadManager::writePermissionRules(const std::string& threadId,
                                         const ThreadPermissionRules& rules) {
    auto conn = acquireConnection(basePath_);

    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();

    rapidjson::Value commandRules(rapidjson::kArrayType);
    for (const auto& rule : rules.commandAllowRules) {
        rapidjson::Value entry(rapidjson::kObjectType);
        entry.AddMember("exact_command",
                        rapidjson::Value(rule.exactCommand.c_str(), a).Move(), a);
        entry.AddMember("normalized_command",
                        rapidjson::Value(rule.normalizedCommand.c_str(), a).Move(), a);
        entry.AddMember("primary_command",
                        rapidjson::Value(rule.primaryCommand.c_str(), a).Move(), a);
        entry.AddMember("severity",
                        rapidjson::Value(severityToString(rule.severity), a).Move(),
                        a);
        commandRules.PushBack(entry, a);
    }
    d.AddMember("command_allow_rules", commandRules, a);

    rapidjson::Value writePaths(rapidjson::kArrayType);
    for (const auto& pathPrefix : rules.writeAllowPaths) {
        writePaths.PushBack(rapidjson::Value(pathPrefix.c_str(), a).Move(), a);
    }
    d.AddMember("write_allow_paths", writePaths, a);

    writeThreadStateField(conn->db, threadId, "permission_rules_json",
                          rapidJsonToString(d));
}

void ThreadManager::addCommandAllowRule(const std::string& threadId,
                                        const CommandAllowRule& rule) {
    auto rules = readPermissionRules(threadId);
    auto exists = std::any_of(
        rules.commandAllowRules.begin(), rules.commandAllowRules.end(),
        [&rule](const CommandAllowRule& existing) {
            return existing.exactCommand == rule.exactCommand &&
                   existing.normalizedCommand == rule.normalizedCommand;
        });
    if (!exists) {
        rules.commandAllowRules.push_back(rule);
        writePermissionRules(threadId, rules);
    }
}

void ThreadManager::addWriteAllowPath(const std::string& threadId,
                                      const std::string& pathPrefix) {
    auto rules = readPermissionRules(threadId);
    auto exists =
        std::any_of(rules.writeAllowPaths.begin(), rules.writeAllowPaths.end(),
                    [&pathPrefix](const std::string& existing) {
                        return existing == pathPrefix;
                    });
    if (!exists) {
        rules.writeAllowPaths.push_back(pathPrefix);
        writePermissionRules(threadId, rules);
    }
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
        "SELECT created_at FROM artifacts WHERE thread_id=? AND owner_agent_id=? AND filename=?;",
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

    Statement upsert(
        conn->db,
        "INSERT INTO artifacts(thread_id, owner_agent_id, owner_friendly_name, filename, "
        "storage_path, content, kind, description, created_at, updated_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(thread_id, owner_agent_id, filename) DO UPDATE SET "
        "owner_friendly_name=excluded.owner_friendly_name, "
        "storage_path=excluded.storage_path, "
        "content=excluded.content, "
        "kind=COALESCE(excluded.kind, artifacts.kind), "
        "description=COALESCE(excluded.description, artifacts.description), "
        "updated_at=excluded.updated_at;",
        "Failed to prepare artifact upsert");

    bindText(upsert.get(), 1, threadId);
    bindText(upsert.get(), 2, ownerAgentId);
    bindText(upsert.get(), 3, ownerFriendlyName);
    bindText(upsert.get(), 4, filename);
    bindText(upsert.get(), 5, storagePath);
    bindText(upsert.get(), 6, content);
    bindOptionalText(upsert.get(), 7, kind);
    bindOptionalText(upsert.get(), 8, description);
    sqlite3_bind_int64(upsert.get(), 9, static_cast<sqlite3_int64>(createdAt));
    sqlite3_bind_int64(upsert.get(), 10, static_cast<sqlite3_int64>(timestamp));

    if (sqlite3_step(upsert.get()) != SQLITE_DONE) {
        throwSqliteError(conn->db, "Failed to write artifact");
    }

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
    if (kind.has_value()) {
        metadata.kind = kind;
    }
    if (description.has_value()) {
        metadata.description = description;
    }

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
                   "SELECT content FROM artifacts WHERE thread_id=? AND owner_agent_id=? AND filename=?;",
                   "Failed to prepare artifact read");
    bindText(stmt.get(), 1, threadId);
    bindText(stmt.get(), 2, ownerAgentId);
    bindText(stmt.get(), 3, filename);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error("Artifact not found: " + ownerAgentId + "/" +
                                 filename);
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
        "SELECT owner_agent_id, owner_friendly_name, filename, storage_path, kind, description, created_at, updated_at "
        "FROM artifacts WHERE thread_id=? ORDER BY owner_friendly_name ASC, filename ASC;",
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
        if (kind) {
            m.kind = std::string(kind);
        }
        if (description) {
            m.description = std::string(description);
        }
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

    auto all = listArtifacts(threadId);
    std::vector<shared::ThreadArtifactMetadata> filtered;
    for (const auto& artifact : all) {
        if (artifact.ownerAgentId == ownerAgentId) {
            filtered.push_back(artifact);
        }
    }
    return filtered;
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
