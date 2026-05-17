#include "embedding/EmbeddingStore.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <vector>

using firmius::core::embedding::EmbeddingRef;
using firmius::core::embedding::EmbeddingStore;
namespace fs = std::filesystem;

namespace {

std::vector<float> randomVector(size_t dim, std::mt19937 &rng) {
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<float> v(dim);
  for (auto &x : v) x = dist(rng);
  return v;
}

void cleanStore(const std::string &path) {
  std::error_code ec;
  fs::remove_all(path, ec);
  fs::remove(path + ".vec", ec);
}

} // namespace

TEST(EmbeddingStore, AddAndRetrieve) {
  cleanStore("/tmp/test_emb_store_1");
  EmbeddingStore store("/tmp/test_emb_store_1", 64);

  std::mt19937 rng(42);
  auto vec = randomVector(64, rng);
  EmbeddingRef ref{0, 64};

  store.add(ref, vec);
  EXPECT_TRUE(store.has(ref));
  EXPECT_EQ(store.size(), 1u);
}

TEST(EmbeddingStore, SearchReturnsNearestNeighbors) {
  cleanStore("/tmp/test_emb_store_2");
  EmbeddingStore store("/tmp/test_emb_store_2", 64);

  std::mt19937 rng(123);
  std::vector<std::vector<float>> vecs;
  for (uint32_t i = 0; i < 10; ++i) {
    auto v = randomVector(64, rng);
    store.add({i, 64}, v);
    vecs.push_back(v);
  }

  // Search for the first vector - should find itself as nearest
  auto results = store.search(vecs[0], 3);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].first.index, 0u);
  EXPECT_GT(results[0].second, 0.9f); // high similarity with itself
}

TEST(EmbeddingStore, HasReturnsFalseForMissing) {
  cleanStore("/tmp/test_emb_store_3");
  EmbeddingStore store("/tmp/test_emb_store_3", 64);
  EXPECT_FALSE(store.has({999, 64}));
}

TEST(EmbeddingStore, SizeTracksAdditions) {
  cleanStore("/tmp/test_emb_store_4");
  EmbeddingStore store("/tmp/test_emb_store_4", 64);

  std::mt19937 rng(456);
  for (uint32_t i = 0; i < 5; ++i) {
    store.add({i, 64}, randomVector(64, rng));
  }
  EXPECT_EQ(store.size(), 5u);
}
