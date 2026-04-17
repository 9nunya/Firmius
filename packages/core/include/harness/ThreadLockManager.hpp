#pragma once

#include <map>
#include <mutex>
#include <string>

namespace firmius::core {

/**
 * Manages process-level file locks for thread directories.
 * Each thread gets an exclusive lock file to prevent concurrent access
 * from multiple Firmius processes.
 *
 * POSIX uses flock-based locking, while Windows uses a file-lock backend.
 */
class ThreadLockManager {
public:
  /**
   * Acquire an exclusive lock on a thread's lock file.
   * Writes the current PID into the lock file on success.
   * If the lock file advertises a dead PID, the stale lock file is removed
   * and acquisition is retried.
   * @param threadId The thread ID to lock
   * @return File descriptor (>= 0) on success, -1 on error, -2 if already
   * locked
   */
  int acquire(const std::string &threadId);

  /**
   * Release a single thread lock and close its file descriptor.
   * @param threadId The thread ID to unlock
   */
  void release(const std::string &threadId);

  /**
   * Release all held thread locks.
   */
  void releaseAll();

  /**
   * Check whether a thread is currently locked by this process.
   * @param threadId The thread ID to check
   * @return true if we hold the lock
   */
  bool isLocked(const std::string &threadId) const;

  /**
   * Read the PID stored in a thread's lock file.
   * @param threadId The thread ID to inspect
   * @return The owning PID, or -1 if unreadable
   */
  int getOwnerPid(const std::string &threadId) const;

  /**
   * Number of locks currently held.
   */
  size_t size() const;

private:
  std::map<std::string, int> locks_;
  mutable std::mutex mutex_;
};

} // namespace firmius::core
