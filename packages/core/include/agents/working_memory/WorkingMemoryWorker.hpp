#ifndef FIRMIUS_CORE_WORKINGMEMORYWORKER_HPP
#define FIRMIUS_CORE_WORKINGMEMORYWORKER_HPP

#include "Context.hpp"
#include "agents/working_memory/DeflationArchive.hpp"
#include "agents/working_memory/Deflator.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace firmius::core {
class Journaler;
} // namespace firmius::core

namespace firmius::core::working_memory {

/**
 * @brief Per-thread background worker for the working-memory layer.
 *
 * Owns:
 *   - A DeflationArchive rooted under <thread_dir>/working_memory/archive.
 *   - A deflation work queue (background jthread). Items: (turnId, partLocator,
 *     toolName, body) tuples. The worker calls the configured summarizer
 *     and the result is published back to the journal via a write-back
 *     callback the caller registers.
 *   - An embedding-text queue. As turns are appended, callers enqueue
 *     (turnId, queryableText). The worker computes embeddings via an
 *     injectable embed function and stores them in a per-thread index that
 *     is queryable for relevance fill.
 *   - A relevance-query interface: given a query string and topK, returns
 *     turn IDs from the embedded set most similar to the query.
 *
 * This class is thread-safe. Construction lazily creates the underlying
 * directories. Destruction waits for queues to drain (with a bounded
 * timeout so shutdown stays bounded).
 */
class ThreadWorkingMemoryWorker {
public:
  using EmbedFn = std::function<std::vector<float>(const std::string&)>;

  ThreadWorkingMemoryWorker(std::string threadId, std::string threadDirPath,
                            EmbedFn embedFn);
  ~ThreadWorkingMemoryWorker();

  ThreadWorkingMemoryWorker(const ThreadWorkingMemoryWorker&) = delete;
  ThreadWorkingMemoryWorker& operator=(const ThreadWorkingMemoryWorker&) =
      delete;

  /// Access the deflation archive for this thread.
  DeflationArchive& archive() { return archive_; }

  /// Enqueue a turn's queryable text for embedding. Returns immediately.
  /// The text should already be a compact representation suitable for
  /// retrieval (tool result summary line, user/assistant text, etc.).
  void enqueueEmbedding(const std::string& turnId, std::string text);

  /// Synchronously query the embedded set for relevant turn IDs. Returns
  /// up to topK turn IDs, ordered by relevance descending. Empty vector
  /// if no embeddings are available yet.
  ///
  /// This method may be called on the agent thread; it must complete
  /// quickly. Embedding the query string itself is delegated to the
  /// caller-supplied embedFn passed at construction.
  std::vector<std::string> queryRelevant(const std::string& query,
                                         std::size_t topK);

  /// Snapshot how many embedding items are pending in the queue. For audits.
  std::size_t embeddingQueueDepth() const;

  /// Snapshot how many embedded turns we have indexed. For audits.
  std::size_t embeddedTurnCount() const;

  /// Force-drain the embedding queue (waits until empty or timeout).
  /// Returns true if drain completed within the deadline.
  bool drainEmbedding(std::chrono::milliseconds timeout);

  /// One pending async deflation upgrade.
  ///
  /// Synchronous-pass code emits a deterministic stub for an oversize
  /// tool-result body, mints an archiveId, and enqueues this job. The
  /// background worker calls the summarizer, then re-acquires the history
  /// mutex to mutate the in-memory history's turn-message-part body in
  /// place, writes the archive, and triggers a journal rewrite for the
  /// affected turn so the upgrade survives restart.
  struct DeflationJob {
    std::string turnId;
    std::size_t messageIndex = 0;
    std::size_t partIndex = 0;
    std::string toolName;
    std::string toolArgs;
    std::string body;       ///< Original body to summarize (already moved into archive).
    std::string archiveId;  ///< Archive entry holding the original body.
    std::uint32_t budgetTokens = 256;
    SummarizerFn summarizer;

    /// History pointer + mutex the worker locks before mutating the in-memory
    /// AgentHistory. Both must be valid for the lifetime of the job.
    std::shared_ptr<shared::AgentHistory> history;
    std::mutex* historyMutex = nullptr;

    /// Optional journaler; when non-null, the worker triggers a rewrite of
    /// the in-memory history after upgrading the part body.
    std::shared_ptr<Journaler> journaler;
  };

  /// Enqueue a deflation upgrade job. Returns immediately. Idempotent on
  /// (turnId, messageIndex, partIndex): a second enqueue for the same
  /// part is dropped while the first is pending.
  void enqueueDeflation(DeflationJob job);

  /// Snapshot the deflation upgrade queue depth. For audits.
  std::size_t deflationQueueDepth() const;

  /// Number of deflation upgrade jobs that have completed (success or
  /// failure). For audits.
  std::size_t deflationJobsCompleted() const;

  /// Number of deflation upgrade jobs that produced a non-empty LLM
  /// summary and were applied successfully. For audits.
  std::size_t deflationJobsApplied() const;

  /// Drain the deflation queue (waits until empty or timeout).
  bool drainDeflation(std::chrono::milliseconds timeout);

  /// Stop background workers; idempotent.
  void shutdown();

private:
  struct EmbedItem {
    std::string turnId;
    std::string text;
  };

  void embedRunLoop();
  void deflateRunLoop();
  void runDeflationJob(DeflationJob job);

  std::string threadId_;
  std::string threadDirPath_;
  DeflationArchive archive_;
  EmbedFn embedFn_;

  // Embedding state. We keep an in-memory map from turnId -> embedding so
  // queryRelevant can do a brute-force cosine ranking without depending on
  // the on-disk HNSW. The HNSW store is still useful for very large threads
  // and is wired in lazily; for the common case of <2k turns the brute
  // force is fast (a few hundred microseconds).
  mutable std::mutex embedMu_;
  std::unordered_map<std::string, std::vector<float>> turnIdToEmbedding_;
  std::queue<EmbedItem> embedQueue_;
  std::condition_variable embedCv_;
  std::condition_variable embedDrainCv_;
  std::atomic<bool> shutdown_{false};
  std::atomic<bool> draining_{false};
  std::jthread embedThread_;

  // Deflation upgrade state.
  mutable std::mutex deflateMu_;
  std::queue<DeflationJob> deflateQueue_;
  std::condition_variable deflateCv_;
  std::condition_variable deflateDrainCv_;
  std::atomic<bool> deflateDraining_{false};
  std::atomic<std::size_t> deflateCompleted_{0};
  std::atomic<std::size_t> deflateApplied_{0};
  // Pending dedup: (turnId|messageIndex|partIndex) — set on enqueue,
  // cleared on completion. Prevents re-enqueuing the same part while a
  // worker is processing it.
  std::unordered_map<std::string, bool> deflatePending_;
  std::jthread deflateThread_;
};

/**
 * @brief Singleton process-wide registry of per-thread workers.
 *
 * One worker per (threadId, agentId) pair. The Engine/Harness obtains
 * workers lazily as they're needed; teardown happens when the thread is
 * deleted or the process shuts down.
 */
class WorkingMemoryWorker {
public:
  static WorkingMemoryWorker& instance();

  /// Get-or-create the worker for the given thread. The caller supplies
  /// the thread directory path (typically ThreadManager::threadDirectoryPath)
  /// and an embed function. The embed function is only used if the worker
  /// is being created; existing workers retain their original embedFn.
  std::shared_ptr<ThreadWorkingMemoryWorker> forThread(
      const std::string& threadId, const std::string& threadDirPath,
      ThreadWorkingMemoryWorker::EmbedFn embedFn);

  /// Drop the worker for a thread (e.g. on thread deletion). Triggers
  /// background shutdown of the worker's embedding thread.
  void releaseThread(const std::string& threadId);

  /// Force shutdown of all workers. Called from Engine::shutdown.
  void shutdownAll();

private:
  WorkingMemoryWorker() = default;

  std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<ThreadWorkingMemoryWorker>>
      workers_;
};

/// Build a deterministic, stable embedding function suitable for tests and
/// audits. Hashes the input text into a fixed-dimension vector so two calls
/// with the same input produce the same output, while different inputs
/// produce orthogonal-ish vectors. Not cryptographically meaningful — only
/// useful for retrieval-quality testing and audit reproducibility.
ThreadWorkingMemoryWorker::EmbedFn deterministicEmbedFn(
    std::uint32_t dimension = 64);

} // namespace firmius::core::working_memory

#endif
