#include "agents/working_memory/WorkingMemoryWorker.hpp"

#include "agents/working_memory/Deflator.hpp"
#include "persistence/Journaler.hpp"
#include "utils/MathUtil.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace firmius::core::working_memory {

namespace {

// xorshift64-based deterministic byte hash.
inline std::uint64_t splitMix64(std::uint64_t z) {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

} // namespace

ThreadWorkingMemoryWorker::EmbedFn deterministicEmbedFn(std::uint32_t dimension) {
  if (dimension == 0) dimension = 64;
  return [dimension](const std::string& text) {
    std::vector<float> v(dimension, 0.0f);
    if (text.empty()) {
      return v;
    }
    // Tokenize to lowercase alpha-num-underscore words; for each word, map
    // a hash of the word to an index modulo dimension and accumulate. This
    // produces a bag-of-words style vector that captures word presence and
    // is reproducible across runs and machines.
    std::string current;
    auto flush = [&]() {
      if (current.empty()) return;
      std::uint64_t h = 1469598103934665603ULL; // FNV-like seed
      for (unsigned char c : current) {
        h ^= c;
        h *= 1099511628211ULL;
      }
      h = splitMix64(h);
      const std::size_t idx =
          static_cast<std::size_t>(h % static_cast<std::uint64_t>(dimension));
      const float sign = (splitMix64(h) & 1ULL) ? 1.0f : -1.0f;
      v[idx] += sign;
      current.clear();
    };
    for (char c : text) {
      const unsigned char uc = static_cast<unsigned char>(c);
      if (std::isalnum(uc) || c == '_') {
        current.push_back(static_cast<char>(std::tolower(uc)));
      } else {
        flush();
      }
    }
    flush();
    // L2-normalize so cosine similarity behaves well.
    float norm = 0.0f;
    for (float x : v) norm += x * x;
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
      for (auto& x : v) x /= norm;
    }
    return v;
  };
}

ThreadWorkingMemoryWorker::ThreadWorkingMemoryWorker(
    std::string threadId, std::string threadDirPath, EmbedFn embedFn)
    : threadId_(std::move(threadId)),
      threadDirPath_(std::move(threadDirPath)),
      archive_(threadDirPath_),
      embedFn_(std::move(embedFn)) {
  if (embedFn_) {
    embedThread_ = std::jthread([this](std::stop_token) { embedRunLoop(); });
  }
  // Always spawn the deflation upgrade thread; jobs are pushed only when a
  // summarizer is configured and the synchronous deflation path emits a
  // deterministic stub. With no jobs, the thread parks on the condvar.
  deflateThread_ = std::jthread([this](std::stop_token) { deflateRunLoop(); });
}

ThreadWorkingMemoryWorker::~ThreadWorkingMemoryWorker() { shutdown(); }

void ThreadWorkingMemoryWorker::shutdown() {
  if (shutdown_.exchange(true)) {
    return;
  }
  embedCv_.notify_all();
  deflateCv_.notify_all();
  if (embedThread_.joinable()) {
    embedThread_.request_stop();
    embedThread_.join();
  }
  if (deflateThread_.joinable()) {
    deflateThread_.request_stop();
    deflateThread_.join();
  }
}

void ThreadWorkingMemoryWorker::enqueueEmbedding(const std::string& turnId,
                                                 std::string text) {
  if (!embedFn_ || turnId.empty() || text.empty()) {
    return;
  }
  {
    std::lock_guard lk(embedMu_);
    if (shutdown_.load()) return;
    embedQueue_.push({turnId, std::move(text)});
  }
  embedCv_.notify_one();
}

std::vector<std::string> ThreadWorkingMemoryWorker::queryRelevant(
    const std::string& query, std::size_t topK) {
  if (!embedFn_ || query.empty() || topK == 0) {
    return {};
  }
  std::vector<float> q;
  try {
    q = embedFn_(query);
  } catch (...) {
    return {};
  }
  if (q.empty()) {
    return {};
  }
  std::vector<std::pair<float, std::string>> scored;
  {
    std::lock_guard lk(embedMu_);
    scored.reserve(turnIdToEmbedding_.size());
    for (const auto& [turnId, vec] : turnIdToEmbedding_) {
      const float s = firmius::shared::cosineSimilarity(q, vec);
      scored.emplace_back(s, turnId);
    }
  }
  if (scored.empty()) {
    return {};
  }
  const std::size_t k = std::min(topK, scored.size());
  std::partial_sort(
      scored.begin(), scored.begin() + k, scored.end(),
      [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<std::string> out;
  out.reserve(k);
  for (std::size_t i = 0; i < k; ++i) {
    if (scored[i].first <= 0.0f) {
      // Drop entries with non-positive similarity; they're worse than a
      // random pick and would just dilute the request.
      break;
    }
    out.push_back(std::move(scored[i].second));
  }
  return out;
}

std::size_t ThreadWorkingMemoryWorker::embeddingQueueDepth() const {
  std::lock_guard lk(embedMu_);
  return embedQueue_.size();
}

std::size_t ThreadWorkingMemoryWorker::embeddedTurnCount() const {
  std::lock_guard lk(embedMu_);
  return turnIdToEmbedding_.size();
}

bool ThreadWorkingMemoryWorker::drainEmbedding(
    std::chrono::milliseconds timeout) {
  if (!embedFn_) return true;
  std::unique_lock lk(embedMu_);
  draining_.store(true);
  const bool ok = embedDrainCv_.wait_for(
      lk, timeout, [this] { return embedQueue_.empty() || shutdown_.load(); });
  draining_.store(false);
  return ok;
}

void ThreadWorkingMemoryWorker::embedRunLoop() {
  while (!shutdown_.load()) {
    EmbedItem item;
    {
      std::unique_lock lk(embedMu_);
      embedCv_.wait(
          lk, [this] { return shutdown_.load() || !embedQueue_.empty(); });
      if (shutdown_.load() && embedQueue_.empty()) {
        embedDrainCv_.notify_all();
        return;
      }
      item = std::move(embedQueue_.front());
      embedQueue_.pop();
    }
    std::vector<float> v;
    try {
      v = embedFn_(item.text);
    } catch (...) {
      v.clear();
    }
    if (!v.empty()) {
      std::lock_guard lk(embedMu_);
      turnIdToEmbedding_[item.turnId] = std::move(v);
    }
    {
      std::lock_guard lk(embedMu_);
      if (embedQueue_.empty() && draining_.load()) {
        embedDrainCv_.notify_all();
      }
    }
  }
}

namespace {
std::string deflationDedupKey(const std::string& turnId, std::size_t mi,
                              std::size_t pi) {
  std::string s = turnId;
  s.push_back('|');
  s += std::to_string(mi);
  s.push_back('|');
  s += std::to_string(pi);
  return s;
}
} // namespace

void ThreadWorkingMemoryWorker::enqueueDeflation(DeflationJob job) {
  if (shutdown_.load()) return;
  if (!job.summarizer) return; // No upgrade possible without a summarizer.
  if (!job.history) return;
  if (!job.historyMutex) return;
  if (job.archiveId.empty() || job.body.empty()) return;
  const std::string key =
      deflationDedupKey(job.turnId, job.messageIndex, job.partIndex);
  {
    std::lock_guard lk(deflateMu_);
    if (deflatePending_.count(key) > 0) {
      return; // already pending; second attempts during the same window
              // would just produce duplicate work.
    }
    deflatePending_[key] = true;
    deflateQueue_.push(std::move(job));
  }
  deflateCv_.notify_one();
}

std::size_t ThreadWorkingMemoryWorker::deflationQueueDepth() const {
  std::lock_guard lk(deflateMu_);
  return deflateQueue_.size();
}

std::size_t ThreadWorkingMemoryWorker::deflationJobsCompleted() const {
  return deflateCompleted_.load();
}

std::size_t ThreadWorkingMemoryWorker::deflationJobsApplied() const {
  return deflateApplied_.load();
}

bool ThreadWorkingMemoryWorker::drainDeflation(
    std::chrono::milliseconds timeout) {
  std::unique_lock lk(deflateMu_);
  deflateDraining_.store(true);
  const bool ok = deflateDrainCv_.wait_for(
      lk, timeout, [this] { return deflateQueue_.empty() || shutdown_.load(); });
  deflateDraining_.store(false);
  return ok;
}

void ThreadWorkingMemoryWorker::deflateRunLoop() {
  while (!shutdown_.load()) {
    DeflationJob job;
    {
      std::unique_lock lk(deflateMu_);
      deflateCv_.wait(lk, [this] {
        return shutdown_.load() || !deflateQueue_.empty();
      });
      if (shutdown_.load() && deflateQueue_.empty()) {
        deflateDrainCv_.notify_all();
        return;
      }
      job = std::move(deflateQueue_.front());
      deflateQueue_.pop();
    }
    runDeflationJob(std::move(job));
    {
      std::lock_guard lk(deflateMu_);
      if (deflateQueue_.empty() && deflateDraining_.load()) {
        deflateDrainCv_.notify_all();
      }
    }
  }
}

void ThreadWorkingMemoryWorker::runDeflationJob(DeflationJob job) {
  // Generate the LLM summary off the agent's hot path. This is the
  // expensive call (typically 1-5s) that we deliberately moved here.
  std::string summary;
  std::atomic<bool> abort{false};
  try {
    summary = job.summarizer(job.toolName, job.toolArgs, job.body,
                             job.budgetTokens, &abort);
  } catch (...) {
    summary.clear();
  }

  const std::string dedupKey =
      deflationDedupKey(job.turnId, job.messageIndex, job.partIndex);

  // Always remove from the pending dedup set and bump the completed
  // counter, regardless of success.
  auto markDone = [&](bool applied) {
    std::lock_guard lk(deflateMu_);
    deflatePending_.erase(dedupKey);
    deflateCompleted_.fetch_add(1);
    if (applied) deflateApplied_.fetch_add(1);
  };

  if (summary.empty()) {
    markDone(false);
    return;
  }

  // Build the upgraded stub body. Format mirrors the synchronous
  // summary stub so isDeflatedStub still returns true and downstream
  // re-deflation passes skip it.
  std::ostringstream upgraded;
  upgraded << "[deflated:";
  if (!job.toolName.empty()) {
    upgraded << ' ' << job.toolName;
  }
  upgraded << " result | archive:" << job.archiveId << "] ";
  std::string s = summary;
  if (s.size() > 1024) {
    s.resize(1024);
    s += "...";
  }
  upgraded << s;
  const std::string newBody = upgraded.str();

  // Apply the upgrade in place under the agent's history mutex. We re-find
  // the turn by ID because turn indices may have shifted since the job
  // was enqueued (new turns appended). If we can't find it, skip
  // gracefully — the deterministic stub stays in place, no harm done.
  bool applied = false;
  std::vector<shared::AgentTurn> snapshot;
  {
    std::lock_guard hlk(*job.historyMutex);
    if (!job.history) {
      markDone(false);
      return;
    }
    auto& turns = job.history->turns;
    for (auto& turn : turns) {
      if (turn.turnId != job.turnId) continue;
      if (job.messageIndex >= turn.messages.size()) break;
      auto& msg = turn.messages[job.messageIndex];
      if (job.partIndex >= msg.content.size()) break;
      auto* tr =
          std::get_if<shared::ToolResultContent>(&msg.content[job.partIndex]);
      if (!tr) break;
      // Only upgrade if the current body is still a deflated stub. If
      // the agent changed the part out from under us (e.g. via undo),
      // leave it alone.
      if (!isDeflatedStub(tr->result)) break;
      tr->result = newBody;
      applied = true;
      break;
    }
    if (applied && job.journaler) {
      // Snapshot under the lock so the journal write reflects exactly the
      // history state we just upgraded.
      snapshot = turns;
    }
  }

  if (applied && job.journaler) {
    try {
      job.journaler->rewriteJournal(snapshot);
    } catch (...) {
      // Journal write failure is non-fatal; the in-memory upgrade still
      // helps the next request.
    }
  }
  markDone(applied);
}

WorkingMemoryWorker& WorkingMemoryWorker::instance() {
  static WorkingMemoryWorker w;
  return w;
}

std::shared_ptr<ThreadWorkingMemoryWorker> WorkingMemoryWorker::forThread(
    const std::string& threadId, const std::string& threadDirPath,
    ThreadWorkingMemoryWorker::EmbedFn embedFn) {
  std::lock_guard lk(mu_);
  auto it = workers_.find(threadId);
  if (it != workers_.end()) {
    return it->second;
  }
  auto worker = std::make_shared<ThreadWorkingMemoryWorker>(
      threadId, threadDirPath, std::move(embedFn));
  workers_[threadId] = worker;
  return worker;
}

void WorkingMemoryWorker::releaseThread(const std::string& threadId) {
  std::shared_ptr<ThreadWorkingMemoryWorker> worker;
  {
    std::lock_guard lk(mu_);
    auto it = workers_.find(threadId);
    if (it == workers_.end()) return;
    worker = std::move(it->second);
    workers_.erase(it);
  }
  if (worker) {
    worker->shutdown();
  }
}

void WorkingMemoryWorker::shutdownAll() {
  std::vector<std::shared_ptr<ThreadWorkingMemoryWorker>> all;
  {
    std::lock_guard lk(mu_);
    for (auto& [_, w] : workers_) {
      all.push_back(std::move(w));
    }
    workers_.clear();
  }
  for (auto& w : all) {
    if (w) w->shutdown();
  }
}

} // namespace firmius::core::working_memory
