#ifndef FIRMIUS_CORE_DEFLATION_ARCHIVE_HPP
#define FIRMIUS_CORE_DEFLATION_ARCHIVE_HPP

#include <mutex>
#include <optional>
#include <string>

namespace firmius::core::working_memory {

/**
 * @brief Per-thread file-backed key-value store for archived tool-result
 * bodies that have been deflated out of the request history.
 *
 * The agent's request history retains the ToolResultContent envelope (id,
 * toolCallId, success flag) and a short stub. The full original result body
 * lives here, addressable by archiveId. Restorable on demand for verbatim
 * recall, replay, or audit.
 *
 * Storage layout: <thread_dir>/working_memory/archive/<archiveId>.txt
 * One file per archived part. Atomic writes via a temp-then-rename dance
 * matching the Firmius persistence pattern.
 */
class DeflationArchive {
public:
  /// Construct an archive rooted under the given thread directory.
  explicit DeflationArchive(std::string threadDirPath);

  /// Generate a fresh archiveId of the form "ar-<thread>-<timestampMs>-<seq>".
  std::string mintId(const std::string& threadId);

  /// Persist a body under the given archiveId. Overwrites any existing entry.
  void put(const std::string& archiveId, const std::string& body);

  /// Retrieve a body. Returns nullopt if missing or unreadable.
  std::optional<std::string> get(const std::string& archiveId) const;

  /// True if an entry exists for the given archiveId.
  bool has(const std::string& archiveId) const;

  /// Remove an entry. No-op if missing.
  void remove(const std::string& archiveId);

  /// Total bytes occupied by archive entries on disk. For audit / metrics.
  std::uint64_t totalBytesOnDisk() const;

  /// Path to the archive root directory, lazily created.
  const std::string& rootDir() const { return rootDir_; }

private:
  void ensureRootExists() const;

  std::string rootDir_;
  mutable std::mutex mu_;
  std::uint64_t seq_ = 0;
};

} // namespace firmius::core::working_memory

#endif
