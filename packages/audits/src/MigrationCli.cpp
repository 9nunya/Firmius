#include "MigrationCli.hpp"

#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"

#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace firmius::audits {
namespace {

void requireValue(int argc, char **argv, int &i, const std::string &flag,
                  std::string &out) {
  if (i + 1 >= argc) {
    throw std::runtime_error("Missing value for " + flag);
  }
  out = argv[++i];
}

void execSql(sqlite3 *db, const std::string &sql, const std::string &context) {
  char *err = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "sqlite error";
    if (err)
      sqlite3_free(err);
    throw std::runtime_error(context + ": " + message);
  }
}

void validateOptions(const MigrationCliOptions &options) {
  if (options.inputDbPath.empty()) {
    throw std::runtime_error("--input-db is required");
  }
  if (!options.applyInPlace && options.outputDbPath.empty()) {
    throw std::runtime_error(
        "Dry-run mode requires --output-db; use --apply-in-place to write to the input DB");
  }
  if (options.applyInPlace && !options.outputDbPath.empty()) {
    throw std::runtime_error(
        "Use either --apply-in-place or --output-db, not both");
  }
}

void copyDatabaseFile(const std::string &source, const std::string &dest) {
  const auto destPath = std::filesystem::path(dest);
  if (destPath.has_parent_path()) {
    std::filesystem::create_directories(destPath.parent_path());
  }
  std::filesystem::copy_file(source, dest,
                             std::filesystem::copy_options::overwrite_existing);
}

void dropThreadV2Data(sqlite3 *db, const std::string &threadId) {
  const std::vector<std::string> statements = {
      "DELETE FROM message_parts_v2 WHERE message_row_id IN (SELECT tm.id FROM turn_messages_v2 tm JOIN agent_turns_v2 at ON at.id=tm.turn_row_id WHERE at.thread_id='" +
          threadId + "');",
      "DELETE FROM turn_messages_v2 WHERE turn_row_id IN (SELECT id FROM agent_turns_v2 WHERE thread_id='" +
          threadId + "');",
      "DELETE FROM agent_turns_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM agent_manifest_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM plans_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM work_tasks_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM work_chunk_files_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM work_chunk_dependencies_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM work_chunks_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM todo_items_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM agent_todos_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM agent_live_state_v2 WHERE thread_id='" + threadId + "';",

      "DELETE FROM fleet_lock_lists_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM fleet_locks_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM compaction_snapshot_turns_v2 WHERE snapshot_row_id IN (SELECT id FROM compaction_snapshots_v2 WHERE thread_id='" +
          threadId + "');",
      "DELETE FROM compaction_snapshots_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM permission_command_rules_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM permission_path_rules_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM permission_tool_sessions_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM permission_state_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM artifacts_v2 WHERE thread_id='" + threadId + "';",
      "DELETE FROM thread_metadata_v2 WHERE thread_id='" + threadId + "';"};

  for (const auto &sql : statements) {
    execSql(db, sql, "Failed dropping v2 thread data");
  }
}

void setMigrationPending(sqlite3 *db) {
  execSql(db,
          "DELETE FROM schema_meta WHERE key IN ('migration_legacy_to_v2','schema_version');",
          "Failed clearing schema meta");
}

void dropLegacyTables(sqlite3 *db) {
  execSql(db,
          "DROP TABLE IF EXISTS artifacts;"
          "DROP TABLE IF EXISTS compaction_snapshots;"
          "DROP TABLE IF EXISTS agent_live_state;"
          "DROP TABLE IF EXISTS agent_todos;"
          "DROP TABLE IF EXISTS plans;"
          "DROP TABLE IF EXISTS agent_turns;"
          "DROP TABLE IF EXISTS thread_states;"
          "DROP TABLE IF EXISTS threads;",
          "Failed dropping legacy tables from migrated DB");
}

void printScalar(sqlite3 *db, const std::string &label, const std::string &sql) {
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("Failed to prepare summary query: " + label);
  }
  int value = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    value = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  std::cout << label << ": " << value << std::endl;
}

void printSummary(sqlite3 *db, const std::string &threadId) {
  std::cout << "Thread " << threadId << std::endl;
  printScalar(db, "  legacy turns",
              "SELECT COUNT(*) FROM agent_turns WHERE thread_id='" + threadId + "';");
  printScalar(db, "  v2 turns",
              "SELECT COUNT(*) FROM agent_turns_v2 WHERE thread_id='" + threadId + "';");
  printScalar(db, "  v2 manifest rows",
              "SELECT COUNT(*) FROM agent_manifest_v2 WHERE thread_id='" + threadId + "';");
  printScalar(db, "  v2 metadata rows",
              "SELECT COUNT(*) FROM thread_metadata_v2 WHERE thread_id='" + threadId + "';");
}

} // namespace

MigrationCliOptions parseMigrationCliOptions(int argc, char **argv) {
  MigrationCliOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--input-db") {
      requireValue(argc, argv, i, arg, options.inputDbPath);
    } else if (arg == "--output-db") {
      requireValue(argc, argv, i, arg, options.outputDbPath);
    } else if (arg == "--thread-id") {
      std::string threadId;
      requireValue(argc, argv, i, arg, threadId);
      options.threadIds.push_back(threadId);
    } else if (arg == "--apply-in-place") {
      options.applyInPlace = true;
    } else if (arg == "--force-remigrate") {
      options.forceRemigrate = true;
    } else {
      throw std::runtime_error("Unknown argument: " + arg);
    }
  }
  validateOptions(options);
  return options;
}

void runMigrationCli(const MigrationCliOptions &options) {
  const std::string requestedTargetDb =
      options.applyInPlace ? options.inputDbPath : options.outputDbPath;

  std::filesystem::path workingBaseDir;
  std::filesystem::path workingDbPath;
  if (options.applyInPlace) {
    workingDbPath = requestedTargetDb;
    workingBaseDir = std::filesystem::path(requestedTargetDb).parent_path();
  } else {
    workingBaseDir = std::filesystem::temp_directory_path() /
                     ("firmius_migrate_" +
                      firmius::shared::StringUtil::generateUuid());
    std::filesystem::create_directories(workingBaseDir);
    workingDbPath = workingBaseDir / "firmius_threads.db";
    copyDatabaseFile(options.inputDbPath, workingDbPath.string());
  }

  sqlite3 *db = nullptr;
  if (sqlite3_open(workingDbPath.string().c_str(), &db) != SQLITE_OK) {
    const std::string message = db ? sqlite3_errmsg(db) : "sqlite open failed";
    if (db)
      sqlite3_close(db);
    throw std::runtime_error("Failed to open target DB: " + message);
  }

  try {
    if (!options.threadIds.empty()) {
      execSql(db, "BEGIN IMMEDIATE;", "Failed to begin thread remigration transaction");
      for (const auto &threadId : options.threadIds) {
        dropThreadV2Data(db, threadId);
      }
      setMigrationPending(db);
      execSql(db, "COMMIT;", "Failed to commit thread remigration reset");
    } else if (options.forceRemigrate) {
      execSql(db, "BEGIN IMMEDIATE;", "Failed to begin full remigration transaction");
      setMigrationPending(db);
      execSql(db, "COMMIT;", "Failed to commit full remigration reset");
    }
    sqlite3_close(db);
    db = nullptr;

    firmius::core::ThreadManager tm(workingBaseDir.string());
    const auto threadIds = options.threadIds.empty() ? tm.listThreads() : options.threadIds;

    if (!options.applyInPlace) {
      copyDatabaseFile(workingDbPath.string(), requestedTargetDb);
    }

    if (sqlite3_open(requestedTargetDb.c_str(), &db) != SQLITE_OK) {
      const std::string message = db ? sqlite3_errmsg(db) : "sqlite reopen failed";
      if (db)
        sqlite3_close(db);
      throw std::runtime_error("Failed to reopen target DB for cleanup: " + message);
    }
    dropLegacyTables(db);
    sqlite3_close(db);
    db = nullptr;

    std::cout << "Migration target DB: " << requestedTargetDb << std::endl;
    std::cout << "Threads selected: " << threadIds.size() << std::endl;

    if (sqlite3_open(requestedTargetDb.c_str(), &db) != SQLITE_OK) {
      const std::string message = db ? sqlite3_errmsg(db) : "sqlite reopen failed";
      if (db)
        sqlite3_close(db);
      throw std::runtime_error("Failed to reopen target DB: " + message);
    }

    for (const auto &threadId : threadIds) {
      printSummary(db, threadId);
    }
  } catch (...) {
    if (db) {
      sqlite3_close(db);
    }
    if (!options.applyInPlace) {
      std::error_code ec;
      std::filesystem::remove_all(workingBaseDir, ec);
    }
    throw;
  }

  if (db) {
    sqlite3_close(db);
  }
  if (!options.applyInPlace) {
    std::error_code ec;
    std::filesystem::remove_all(workingBaseDir, ec);
  }
}

} // namespace firmius::audits
