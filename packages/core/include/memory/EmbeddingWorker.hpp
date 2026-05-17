#ifndef FIRMIUS_CORE_MEMORY_EMBEDDING_WORKER_HPP
#define FIRMIUS_CORE_MEMORY_EMBEDDING_WORKER_HPP

#include <functional>
#include <memory>
#include <string>

namespace firmius::core::memory {

struct EmbeddingRef;

class EmbeddingWorker {
public:
  using EmbedFn = std::function<std::vector<float>(const std::string &)>;

  EmbeddingWorker(EmbedFn embedFn, const std::string &storePath);
  ~EmbeddingWorker();

  void enqueue(EmbeddingRef ref, const std::string &text);
  void shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace firmius::core::memory

#endif
