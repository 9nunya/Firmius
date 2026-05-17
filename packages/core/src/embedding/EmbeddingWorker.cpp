#include "embedding/EmbeddingWorker.hpp"
#include "embedding/EmbeddingStore.hpp"
#include "embedding/ModelDownloader.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

namespace firmius::core::embedding {

class EmbeddingWorker::Impl {
public:
  Impl(EmbedFn embedFn, const std::string &storePath,
       const std::string &modelId, ProgressCallback onProgress)
      : embedFn_(std::move(embedFn)), store_(storePath),
        modelId_(modelId.empty() ? ModelDownloader::defaultModelId() : modelId),
        onProgress_(std::move(onProgress)),
        worker_([this] { run(); }) {}

  ~Impl() { shutdown(); }

  void enqueue(EmbeddingRef ref, const std::string &text) {
    {
      std::lock_guard lk(mu_);
      if (shutdown_) return;
      queue_.push({ref, text});
    }
    cv_.notify_one();
  }

  void shutdown() {
    {
      std::lock_guard lk(mu_);
      shutdown_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
  }

private:
  struct WorkItem {
    EmbeddingRef ref;
    std::string text;
  };

  void ensureModel() {
    if (modelReady_) return;
    if (ModelDownloader::isModelAvailable(modelId_)) {
      modelReady_ = true;
      return;
    }
    ModelDownloader::ensureModel(modelId_, [this](const DownloadProgress &p) {
      if (onProgress_) onProgress_(p);
    });
    modelReady_ = true;
  }

  void run() {
    for (;;) {
      WorkItem item;
      {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [this] { return shutdown_ || !queue_.empty(); });
        if (shutdown_ && queue_.empty()) return;
        item = std::move(queue_.front());
        queue_.pop();
      }

      ensureModel();

      try {
        auto embedding = embedFn_(item.text);
        if (!embedding.empty()) {
          store_.add(item.ref, embedding);
        }
      } catch (...) {
        // Embedding failed, skip
      }
    }
  }

  EmbedFn embedFn_;
  EmbeddingStore store_;
  std::string modelId_;
  ProgressCallback onProgress_;
  std::atomic<bool> modelReady_{false};
  std::mutex mu_;
  std::condition_variable cv_;
  std::queue<WorkItem> queue_;
  bool shutdown_ = false;
  std::jthread worker_;
};

EmbeddingWorker::EmbeddingWorker(EmbedFn embedFn, const std::string &storePath,
                                 const std::string &modelId,
                                 ProgressCallback onProgress)
    : impl_(std::make_unique<Impl>(std::move(embedFn), storePath, modelId,
                                   std::move(onProgress))) {}

EmbeddingWorker::~EmbeddingWorker() = default;

void EmbeddingWorker::enqueue(EmbeddingRef ref, const std::string &text) {
  impl_->enqueue(ref, text);
}

void EmbeddingWorker::shutdown() {
  impl_->shutdown();
}

} // namespace firmius::core::embedding
