#ifndef FIRMIUS_CORE_MEMORY_RECALL_INDEX_HPP
#define FIRMIUS_CORE_MEMORY_RECALL_INDEX_HPP

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace firmius::core::memory {

using EventId = uint64_t;

struct IndexedEvent {
  EventId id;
  std::string content;
  std::vector<std::string> anchors;
  std::vector<std::string> filePaths;
  uint64_t contentHash;
  uint32_t docLength;
  std::chrono::system_clock::time_point timestamp;
};

struct ScoredEvent {
  EventId id;
  float score;
};

struct Query {
  std::string text;
  std::vector<std::string> activeFiles;
  std::vector<std::string> activeAnchors;
};

class RecallIndex {
public:
  void addEvent(IndexedEvent event);
  std::vector<ScoredEvent> recall(const Query &query, size_t k) const;
  std::optional<std::string> dedup(uint64_t hash) const;
  size_t size() const { return events_.size(); }

private:
  struct Posting {
    EventId eventId;
    uint32_t termFreq;
  };

  std::vector<std::string> tokenize(std::string_view text) const;
  std::vector<std::string> extractFilePaths(std::string_view text) const;
  float bm25Score(EventId eventId,
                  const std::vector<std::string> &terms) const;
  float recencyScore(const IndexedEvent &ev) const;
  float fileMatchScore(const IndexedEvent &ev,
                       const std::vector<std::string> &activeFiles) const;

  static constexpr float k1 = 1.2f;
  static constexpr float b = 0.75f;
  static constexpr float wR = 1.0f;
  static constexpr float wF = 2.0f;
  static constexpr float wB = 1.5f;

  std::vector<IndexedEvent> events_;
  std::unordered_map<EventId, size_t> eventIdx_;
  std::unordered_map<std::string, std::vector<Posting>> inverted_;
  std::unordered_map<std::string, std::unordered_set<EventId>> fileToEvents_;
  std::unordered_map<EventId, std::unordered_set<std::string>> eventToFiles_;
  std::unordered_map<std::string, std::unordered_set<EventId>> anchorToEvents_;
  std::unordered_map<uint64_t, std::string> dedupMap_;
  uint64_t totalDocLength_ = 0;
  size_t nextIdx_ = 0;
};

} // namespace firmius::core::memory

#endif
