#ifndef FIRMIUS_CORE_HOOKSTATE_HPP
#define FIRMIUS_CORE_HOOKSTATE_HPP

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core::hooks {

/**
 * @brief Persistent KV store for hook authors.
 *
 * Hooks declare their state surface in YAML (`state.scope`, `state.reads`,
 * `state.writes`) and read/write it through this API. The store is a
 * JSON document tree indexed by `(scope, key)`:
 *
 *   - `global` → `~/.firmius/hook-state/global.json`
 *   - `thread` → `~/.firmius/threads/<thread_id>/hook-state.json`
 *   - `agent`  → in-memory, attached to AgentContext, persisted alongside
 *                the thread on snapshot.
 *   - `hook`   → private to the declaring hook id (rare; for hook-internal
 *                memoization that survives restarts).
 *
 * Path syntax (read/write):
 *   - `foo.bar.baz`  → object descent
 *   - `foo[]`        → array append (write-only)
 *   - `foo[3]`       → array index access (1-indexed in Luau, 0-indexed in
 *                      JSON path; this API uses 0-indexed C++ semantics)
 *
 * Concurrency: each scope has its own RW mutex. Cross-scope writes are
 * not transactional. A hook that needs atomicity should use a single
 * scope and group its writes via the HookOutcome.stateWrites batch — the
 * dispatcher applies that batch atomically per scope.
 *
 * Persistence: writes are journaled to disk eagerly with atomic-rename
 * (.tmp then rename) so a crash mid-write loses at most the in-flight
 * outcome. Reads are served from memory; the file is the durable record.
 */
class HookState {
public:
  enum class Scope { Global, Thread, Agent, Hook };

  static HookState &instance();

  /// Set the active thread id. Loads the thread-scoped JSON file lazily.
  void bindThread(const std::string &threadId);

  /// Drop in-memory thread state when a thread is closed.
  void unbindThread(const std::string &threadId);

  /// Read a value at `path` within `scope`. Returns nullopt when missing.
  /// `hookId` is required when scope == Hook (for namespacing).
  std::optional<std::string> readJson(Scope scope, const std::string &path,
                                      const std::string &hookId = "") const;

  /// Write a value (`valueJson`) to `path` within `scope`. Creates parent
  /// objects as needed. Returns true on success.
  bool writeJson(Scope scope, const std::string &path,
                 const std::string &valueJson,
                 const std::string &hookId = "");

  /// Append `valueJson` to an array at `path`. The array is created if
  /// missing. Returns true on success.
  bool appendJson(Scope scope, const std::string &path,
                  const std::string &valueJson,
                  const std::string &hookId = "");

  /// Delete the value at `path`. Returns true if something was removed.
  bool deleteJson(Scope scope, const std::string &path,
                  const std::string &hookId = "");

  /// Render the entire state graph as a single JSON object suitable for
  /// embedding in a HookEnvelope. Shape:
  ///   { "global": {...}, "thread": {...}, "agent": {...}, "hook": {...} }
  /// Pass an empty `hookId` to omit the per-hook private slice.
  std::string snapshotJson(const std::string &hookId = "") const;

  /// Apply a batch of writes atomically (per scope). Used by the
  /// dispatcher when settling a HookOutcome.
  struct BatchWrite {
    Scope scope;
    std::string path;
    std::string valueJson;
    bool append = false;  ///< true → array append; false → set
  };
  bool applyBatch(const std::vector<BatchWrite> &writes,
                  const std::string &hookId = "");

  // ─── TODO: scaffolding stubs for upcoming primitives ────────────────────
  /// Time-to-live writes (`writeJsonTTL`), increment helpers, and an
  /// observation channel (`subscribe(path, callback)`) ship in a follow-up
  /// pass. The interface is reserved here so YAML schemas can reference
  /// them without breaking.

  /// Implementation detail; defined in HookState.cpp. Forward-declared
  /// here so private helpers can return / accept pointers without
  /// leaking rapidjson into this public header.
  struct ScopeStore;

private:
  HookState() = default;

  /// Lock-free worker for applyBatch — assumes the caller holds mu_.
  bool applyBatchUnlocked(const std::vector<BatchWrite> &writes,
                          const std::string &hookId);

  mutable std::mutex mu_;
  std::string activeThreadId_;
};

const char *scopeName(HookState::Scope s);
HookState::Scope parseScope(const std::string &name);

} // namespace firmius::core::hooks

#endif
