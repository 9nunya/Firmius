#ifndef FIRMIUS_CORE_EMBEDDINGWORKER_HPP
#define FIRMIUS_CORE_EMBEDDINGWORKER_HPP

#include <functional>
#include <memory>
#include <string>

namespace firmius::core::embedding {

struct EmbeddingRef;
struct DownloadProgress;

class EmbeddingWorker {
public:
  using EmbedFn = std::function<std::vector<float>(const std::string &)>;
  using ProgressCallback = std::function<void(const DownloadProgress &)>;

  /// Construct with embed function and store path.
  /// If modelId is empty, uses defaultModelId() and triggers lazy download.
  /// ProgressCallback receives download progress events.
  EmbeddingWorker(EmbedFn embedFn, const std::string &storePath,
                  const std::string &modelId = "",
                  ProgressCallback onProgress = nullptr);
  ~EmbeddingWorker();

  void enqueue(EmbeddingRef ref, const std::string &text);
  void shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace firmius::core::embedding

#endif
