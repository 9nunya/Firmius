#ifndef FIRMIUS_CORE_EMBEDDINGSTORE_HPP
#define FIRMIUS_CORE_EMBEDDINGSTORE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace firmius::core::embedding {

struct EmbeddingRef {
  uint32_t index; // position in flat file
  uint32_t dim;   // embedding dimension
};

class EmbeddingStore {
public:
  EmbeddingStore(const std::string &path, uint32_t dim = 0);
  ~EmbeddingStore();

  void add(EmbeddingRef ref, const std::vector<float> &embedding);
  std::vector<std::pair<EmbeddingRef, float>>
  search(const std::vector<float> &query, size_t k) const;
  bool has(EmbeddingRef ref) const;
  size_t size() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace firmius::core::embedding

#endif
