#include "embedding/EmbeddingStore.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <random>
#include <unordered_map>

namespace firmius::core::embedding {

namespace {

float cosineSimilarity(const std::vector<float> &a, const std::vector<float> &b) {
  if (a.size() != b.size() || a.empty()) return 0.0f;

  float dot = 0.0f, normA = 0.0f, normB = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    normA += a[i] * a[i];
    normB += b[i] * b[i];
  }

  float denom = std::sqrt(normA) * std::sqrt(normB);
  return denom > 0.0f ? dot / denom : 0.0f;
}

struct HNSWNode {
  EmbeddingRef ref;
  std::vector<float> embedding;
  std::vector<uint32_t> neighbors; // indices into nodes vector
};

} // namespace

class EmbeddingStore::Impl {
public:
  Impl(const std::string &path, uint32_t dim)
      : path_(path), dim_(dim), rng_(std::random_device{}()) {
    loadFromDisk();
  }

  ~Impl() { saveToDisk(); }

  void add(EmbeddingRef ref, const std::vector<float> &embedding) {
    std::lock_guard lk(mu_);
    if (dim_ == 0 && !embedding.empty()) dim_ = static_cast<uint32_t>(embedding.size());
    if (embedding.size() != dim_) return;

    uint32_t nodeIdx = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back({ref, embedding, {}});

    // HNSW insertion: connect to M nearest neighbors
    constexpr uint32_t M = 16;
    auto neighbors = searchInternal(embedding, M);
    for (auto &[nbrRef, score] : neighbors) {
      uint32_t nbrIdx = refToNode_[nbrRef.index];
      nodes_[nodeIdx].neighbors.push_back(nbrIdx);
      nodes_[nbrIdx].neighbors.push_back(nodeIdx);
    }

    refToNode_[ref.index] = nodeIdx;
    dirty_ = true;
  }

  std::vector<std::pair<EmbeddingRef, float>>
  search(const std::vector<float> &query, size_t k) const {
    std::lock_guard lk(mu_);
    return searchInternal(query, k);
  }

  bool has(EmbeddingRef ref) const {
    std::lock_guard lk(mu_);
    return refToNode_.count(ref.index) > 0;
  }

  size_t size() const {
    std::lock_guard lk(mu_);
    return nodes_.size();
  }

private:
  std::vector<std::pair<EmbeddingRef, float>>
  searchInternal(const std::vector<float> &query, size_t k) const {
    if (nodes_.empty() || k == 0) return {};

    // Greedy beam search from random entry point
    uint32_t entry = rng_() % nodes_.size();
    std::vector<std::pair<uint32_t, float>> candidates;
    candidates.emplace_back(entry, cosineSimilarity(query, nodes_[entry].embedding));

    constexpr size_t beamWidth = 64;
    std::vector<bool> visited(nodes_.size(), false);
    visited[entry] = true;

    for (int layer = 0; layer < 3; ++layer) { // 3-layer HNSW
      bool improved = true;
      while (improved) {
        improved = false;
        auto [bestIdx, bestScore] = candidates.back();

        for (uint32_t nbr : nodes_[bestIdx].neighbors) {
          if (visited[nbr]) continue;
          visited[nbr] = true;

          float score = cosineSimilarity(query, nodes_[nbr].embedding);
          candidates.emplace_back(nbr, score);

          if (score > bestScore) {
            bestScore = score;
            improved = true;
          }
        }

        // Keep top beamWidth candidates
        if (candidates.size() > beamWidth) {
          std::partial_sort(candidates.begin(), candidates.begin() + beamWidth,
                           candidates.end(),
                           [](auto &a, auto &b) { return a.second > b.second; });
          candidates.resize(beamWidth);
        }
      }
    }

    // Sort and return top-k
    std::partial_sort(candidates.begin(),
                     candidates.begin() + std::min(k, candidates.size()),
                     candidates.end(),
                     [](auto &a, auto &b) { return a.second > b.second; });

    std::vector<std::pair<EmbeddingRef, float>> results;
    for (size_t i = 0; i < std::min(k, candidates.size()); ++i) {
      results.emplace_back(nodes_[candidates[i].first].ref, candidates[i].second);
    }
    return results;
  }

  void loadFromDisk() {
    std::ifstream in(path_ + ".vec", std::ios::binary);
    if (!in) return;

    uint32_t count, dim;
    in.read(reinterpret_cast<char *>(&count), sizeof(count));
    in.read(reinterpret_cast<char *>(&dim), sizeof(dim));
    if (dim_ == 0) dim_ = dim;
    if (dim != dim_) return;

    nodes_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      EmbeddingRef ref;
      in.read(reinterpret_cast<char *>(&ref), sizeof(ref));

      std::vector<float> emb(dim_);
      in.read(reinterpret_cast<char *>(emb.data()), dim_ * sizeof(float));

      uint32_t nbrCount;
      in.read(reinterpret_cast<char *>(&nbrCount), sizeof(nbrCount));

      std::vector<uint32_t> nbrs(nbrCount);
      in.read(reinterpret_cast<char *>(nbrs.data()), nbrCount * sizeof(uint32_t));

      nodes_.push_back({ref, std::move(emb), std::move(nbrs)});
      refToNode_[ref.index] = i;
    }
  }

  void saveToDisk() {
    if (!dirty_) return;

    std::ofstream out(path_ + ".vec", std::ios::binary);
    if (!out) return;

    uint32_t count = static_cast<uint32_t>(nodes_.size());
    out.write(reinterpret_cast<const char *>(&count), sizeof(count));
    out.write(reinterpret_cast<const char *>(&dim_), sizeof(dim_));

    for (auto &node : nodes_) {
      out.write(reinterpret_cast<const char *>(&node.ref), sizeof(node.ref));
      out.write(reinterpret_cast<const char *>(node.embedding.data()),
               dim_ * sizeof(float));

      uint32_t nbrCount = static_cast<uint32_t>(node.neighbors.size());
      out.write(reinterpret_cast<const char *>(&nbrCount), sizeof(nbrCount));
      out.write(reinterpret_cast<const char *>(node.neighbors.data()),
               nbrCount * sizeof(uint32_t));
    }

    dirty_ = false;
  }

  std::string path_;
  uint32_t dim_;
  mutable std::mt19937 rng_;
  mutable std::mutex mu_;
  std::vector<HNSWNode> nodes_;
  std::unordered_map<uint32_t, uint32_t> refToNode_; // ref.index -> node index
  bool dirty_ = false;
};

EmbeddingStore::EmbeddingStore(const std::string &path, uint32_t dim)
    : impl_(std::make_unique<Impl>(path, dim)) {}

EmbeddingStore::~EmbeddingStore() = default;

void EmbeddingStore::add(EmbeddingRef ref, const std::vector<float> &embedding) {
  impl_->add(ref, embedding);
}

std::vector<std::pair<EmbeddingRef, float>>
EmbeddingStore::search(const std::vector<float> &query, size_t k) const {
  return impl_->search(query, k);
}

bool EmbeddingStore::has(EmbeddingRef ref) const {
  return impl_->has(ref);
}

size_t EmbeddingStore::size() const {
  return impl_->size();
}

} // namespace firmius::core::embedding
