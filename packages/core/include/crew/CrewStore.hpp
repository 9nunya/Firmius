#ifndef FIRMIUS_CORE_CREW_CREWSTORE_HPP
#define FIRMIUS_CORE_CREW_CREWSTORE_HPP

#include "Crew.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::core::crew {

/**
 * @brief Result of a CrewStore mutation. Exceptions are reserved for
 * invariant violations; expected failures (crew not found, etc) return
 * a `Result` with `ok=false`.
 */
struct StoreResult {
  bool ok = false;
  std::string error;            ///< Non-empty when ok=false.

  static StoreResult success() { return {true, {}}; }
  static StoreResult failure(std::string msg) { return {false, std::move(msg)}; }
};

/**
 * @brief File-backed persistent store for Crew state.
 *
 * Layout (one directory per crew):
 *   <firmiusHome>/threads/<threadId>/crews/<crewId>/
 *     ├── manifest.json    (Crew + qualityGates)
 *     ├── members.json     (vector<CrewMember>)
 *     ├── tasks.json       (vector<CrewTask>)
 *     ├── channels.json    (vector<CrewChannel>)
 *     ├── mail.jsonl       (one CrewMail per line)
 *     ├── events.jsonl     (one CrewEvent per line)
 *     └── flags.jsonl      (one CrewFlag per line)
 *
 * The four JSON files (manifest/members/tasks/channels) use the atomic
 * tmp+rename idiom for crash safety. The three JSONL files use append-only
 * writes — torn final-line reads are tolerated by trimming on load.
 *
 * Schema versioning: every JSON file carries a top-level "schema_version"
 * field. Loading a file with an unknown schema version fails loudly
 * rather than silently dropping data.
 *
 * Concurrency model: One CrewStore instance per thread. Per-crew state is
 * guarded by a per-crew std::mutex held for the duration of any read or
 * write. The store does NOT use cross-process locking — the caller (the
 * agent harness) owns the thread lock that prevents two processes from
 * touching the same thread's crews simultaneously.
 */
class CrewStore {
public:
  /**
   * @brief Construct a store rooted at the given thread directory.
   * @param threadId The owning thread's id.
   * @param threadDirectory Path like <firmiusHome>/threads/<threadId>.
   *        If empty, the store resolves it from PlatformPaths.
   */
  CrewStore(std::string threadId, std::filesystem::path threadDirectory = {});

  /// The owning thread.
  const std::string &threadId() const { return threadId_; }
  /// The crews/ directory under the thread.
  const std::filesystem::path &crewsRoot() const { return crewsRoot_; }

  // ── Crew lifecycle ─────────────────────────────────────────────────────

  /// Create a new crew with a fresh crewId. Returns the formed Crew.
  std::optional<Crew> formCrew(const std::string &title,
                               const std::string &brief,
                               std::map<std::string, std::string> meta = {});

  /// Mark a crew as Disbanded. Does not delete files; sets disbandedAtMs.
  StoreResult disbandCrew(const std::string &crewId);

  /// Mark a crew as Paused.
  StoreResult pauseCrew(const std::string &crewId);

  /// Mark a paused crew as Active.
  StoreResult resumeCrew(const std::string &crewId);

  /// Update arbitrary mutable fields on the manifest (status, coordinator,
  /// architect, meta). Caller passes a fully-edited Crew; store overwrites.
  StoreResult writeManifest(const Crew &crew);

  /// Read the manifest.
  std::optional<Crew> readManifest(const std::string &crewId) const;

  /// List every crewId persisted under this thread.
  std::vector<std::string> listCrewIds() const;

  /// Compute a CrewSummary by scanning each per-crew file.
  std::optional<CrewSummary> summarize(const std::string &crewId) const;

  // ── Members ────────────────────────────────────────────────────────────

  std::vector<CrewMember> readMembers(const std::string &crewId) const;
  StoreResult writeMembers(const std::string &crewId,
                           const std::vector<CrewMember> &members);

  /// Add a new member (mints memberId). Returns the inserted record.
  std::optional<CrewMember> enlistMember(const std::string &crewId,
                                         CrewMember newMember);

  /// Mark a member as discharged (sets dischargedAtMs).
  StoreResult dischargeMember(const std::string &crewId,
                              const std::string &memberId);

  // ── Tasks ──────────────────────────────────────────────────────────────

  std::vector<CrewTask> readTasks(const std::string &crewId) const;
  StoreResult writeTasks(const std::string &crewId,
                         const std::vector<CrewTask> &tasks);

  /// Insert a new task (mints taskId). Returns the inserted record.
  std::optional<CrewTask> insertTask(const std::string &crewId,
                                     CrewTask newTask);

  /// Replace a single existing task by taskId. Returns false if not found.
  StoreResult updateTask(const std::string &crewId, const CrewTask &task);

  // ── Channels ───────────────────────────────────────────────────────────

  std::vector<CrewChannel> readChannels(const std::string &crewId) const;
  StoreResult writeChannels(const std::string &crewId,
                            const std::vector<CrewChannel> &channels);

  std::optional<CrewChannel> openChannel(const std::string &crewId,
                                         const std::string &name,
                                         std::vector<std::string> memberIds);

  // ── Mail (append-only) ─────────────────────────────────────────────────

  /// Append one mail to the log. Returns the assigned mailId.
  std::optional<std::string> appendMail(const std::string &crewId,
                                        CrewMail mail);

  /// Read every mail for a crew. Tolerates a torn final line.
  std::vector<CrewMail> readMail(const std::string &crewId) const;

  /// Mark a mail as Ack'd by `byMemberId`. Rewrites the mail log.
  StoreResult ackMail(const std::string &crewId, const std::string &mailId,
                      const std::string &byMemberId);

  // ── Events (append-only) ───────────────────────────────────────────────

  /// Append one event. Mints sequence number. Returns the assigned sequence.
  std::uint64_t appendEvent(const std::string &crewId, CrewEvent event);

  /// Read events for a crew, optionally filtered by sinceSequence.
  std::vector<CrewEvent> readEvents(const std::string &crewId,
                                    std::uint64_t sinceSequence = 0) const;

  // ── Flags (append-only) ────────────────────────────────────────────────

  std::uint64_t appendFlag(const std::string &crewId, CrewFlag flag);

  /// Read flags, optionally filtered by sinceSequence and/or memberId.
  std::vector<CrewFlag> readFlags(const std::string &crewId,
                                  std::uint64_t sinceSequence = 0,
                                  const std::string &memberId = {}) const;

  /// Mark a flag as resolved. Rewrites the flag log.
  StoreResult resolveFlag(const std::string &crewId,
                          const std::string &flagId,
                          const std::string &byMemberId);

private:
  // Per-crew lock. Acquired around any read or write operation.
  std::mutex &lockFor(const std::string &crewId) const;

  // Path helpers.
  std::filesystem::path crewDir(const std::string &crewId) const;
  std::filesystem::path manifestPath(const std::string &crewId) const;
  std::filesystem::path membersPath(const std::string &crewId) const;
  std::filesystem::path tasksPath(const std::string &crewId) const;
  std::filesystem::path channelsPath(const std::string &crewId) const;
  std::filesystem::path mailPath(const std::string &crewId) const;
  std::filesystem::path eventsPath(const std::string &crewId) const;
  std::filesystem::path flagsPath(const std::string &crewId) const;

  std::string threadId_;
  std::filesystem::path crewsRoot_;

  // We need per-crew mutexes that survive lookups. std::mutex is non-movable,
  // so we keep them in a heap-allocated map keyed by crewId.
  mutable std::mutex registryMutex_;
  mutable std::unordered_map<std::string, std::unique_ptr<std::mutex>>
      crewMutexes_;
};

// ── Free helpers (also used by tests + watchdog) ─────────────────────────

/// Generate a short, URL-safe id like "crew-a8f3" / "m-7e1c" / "t-c4b2".
std::string mintShortId(const std::string &prefix);

/// Current epoch milliseconds.
std::uint64_t nowEpochMs();

} // namespace firmius::core::crew

#endif // FIRMIUS_CORE_CREW_CREWSTORE_HPP
