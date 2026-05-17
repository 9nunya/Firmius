#include "memory/RecallIndex.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace firmius::core::memory;

namespace {

IndexedEvent makeEvent(EventId id, const std::string &content,
                       const std::vector<std::string> &anchors = {},
                       const std::vector<std::string> &files = {},
                       std::chrono::system_clock::time_point ts = std::chrono::system_clock::now()) {
  IndexedEvent ev;
  ev.id = id;
  ev.content = content;
  ev.anchors = anchors;
  ev.filePaths = files;
  ev.contentHash = std::hash<std::string>{}(content);
  ev.docLength = static_cast<uint32_t>(content.size() / 4 + 1);
  ev.timestamp = ts;
  return ev;
}

} // namespace

TEST(RecallIndex, BM25RanksRelevantHigher) {
  RecallIndex idx;
  auto t = std::chrono::system_clock::now();
  idx.addEvent(makeEvent(1, "the quick brown fox jumps over the lazy dog", {}, {}, t));
  idx.addEvent(makeEvent(2, "the fox is quick and brown", {}, {}, t));
  idx.addEvent(makeEvent(3, "completely unrelated content about cats", {}, {}, t));

  Query q;
  q.text = "fox brown";
  auto results = idx.recall(q, 10);

  ASSERT_GE(results.size(), 2u);
  EXPECT_EQ(results[0].id, 2u);
  EXPECT_GT(results[0].score, results[1].score);
}

TEST(RecallIndex, FileReferenceGraph) {
  RecallIndex idx;
  idx.addEvent(makeEvent(1, "modified the main file", {}, {"src/main.cpp"}));
  idx.addEvent(makeEvent(2, "edited header", {}, {"include/main.hpp"}));
  idx.addEvent(makeEvent(3, "other work", {}, {}));

  Query q;
  q.text = "";
  q.activeFiles = {"src/main.cpp"};
  auto results = idx.recall(q, 10);

  ASSERT_GE(results.size(), 1u);
  EXPECT_EQ(results[0].id, 1u);
}

TEST(RecallIndex, AnchorIndex) {
  RecallIndex idx;
  idx.addEvent(makeEvent(1, "fixed bug", {"BUG-123"}, {}));
  idx.addEvent(makeEvent(2, "added feature", {"FEAT-456"}, {}));
  idx.addEvent(makeEvent(3, "refactored", {}, {}));

  Query q;
  q.text = "";
  q.activeAnchors = {"BUG-123"};
  auto results = idx.recall(q, 10);

  ASSERT_GE(results.size(), 1u);
  EXPECT_EQ(results[0].id, 1u);
}

TEST(RecallIndex, HybridScoring) {
  RecallIndex idx;
  auto now = std::chrono::system_clock::now();
  auto old = now - std::chrono::hours(24);

  idx.addEvent(makeEvent(1, "old fox reference", {"FOX"}, {"fox.cpp"}, old));
  idx.addEvent(makeEvent(2, "recent fox work", {}, {}, now));
  idx.addEvent(makeEvent(3, "unrelated", {}, {}, now));

  Query q;
  q.text = "fox";
  q.activeFiles = {"fox.cpp"};
  q.activeAnchors = {"FOX"};
  auto results = idx.recall(q, 10);

  ASSERT_GE(results.size(), 2u);
  EXPECT_EQ(results[0].id, 1u);
}

TEST(RecallIndex, DedupReturnsReference) {
  RecallIndex idx;
  idx.addEvent(makeEvent(1, "hello world"));
  idx.addEvent(makeEvent(2, "different content"));

  auto ref = idx.dedup(std::hash<std::string>{}("hello world"));
  ASSERT_TRUE(ref.has_value());
  EXPECT_NE(ref->find("identical"), std::string::npos);

  auto missing = idx.dedup(99999);
  EXPECT_FALSE(missing.has_value());
}

TEST(RecallIndex, EmptyQueryReturnsRecent) {
  RecallIndex idx;
  idx.addEvent(makeEvent(1, "first"));
  idx.addEvent(makeEvent(2, "second"));
  idx.addEvent(makeEvent(3, "third"));

  Query q;
  auto results = idx.recall(q, 2);

  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].id, 3u);
  EXPECT_EQ(results[1].id, 2u);
}

TEST(RecallIndex, Size) {
  RecallIndex idx;
  EXPECT_EQ(idx.size(), 0u);
  idx.addEvent(makeEvent(1, "a"));
  EXPECT_EQ(idx.size(), 1u);
}
