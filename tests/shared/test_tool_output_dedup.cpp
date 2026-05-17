#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

// Test the dedup logic pattern used in Agent::executeTools.
// We replicate the core algorithm here to verify correctness without
// needing to instantiate a full Agent.

namespace {

struct DedupEntry {
  std::string reference;
};

std::string dedupToolResult(
    std::unordered_map<uint64_t, DedupEntry> &dedupMap,
    const std::string &toolName,
    const std::string &result,
    uint64_t eventIndex) {
  std::string dedupKey = toolName + "|" + result;
  uint64_t contentHash = std::hash<std::string>{}(dedupKey);

  auto [it, inserted] = dedupMap.try_emplace(contentHash, DedupEntry{});
  if (!inserted) {
    return it->second.reference;
  }
  it->second.reference =
      "↪ identical to event #" + std::to_string(eventIndex);
  return result;
}

} // namespace

TEST(ToolOutputDedup, FirstOccurrenceReturnsFullResult) {
  std::unordered_map<uint64_t, DedupEntry> dedupMap;
  auto out = dedupToolResult(dedupMap, "read_file", "file contents here", 5);
  EXPECT_EQ(out, "file contents here");
  EXPECT_EQ(dedupMap.size(), 1u);
}

TEST(ToolOutputDedup, DuplicateReturnsReference) {
  std::unordered_map<uint64_t, DedupEntry> dedupMap;
  auto first = dedupToolResult(dedupMap, "read_file", "same content", 1);
  EXPECT_EQ(first, "same content");

  auto second = dedupToolResult(dedupMap, "read_file", "same content", 2);
  EXPECT_EQ(second, "↪ identical to event #1");
  EXPECT_EQ(dedupMap.size(), 1u);
}

TEST(ToolOutputDedup, DifferentResultsNotDeduped) {
  std::unordered_map<uint64_t, DedupEntry> dedupMap;
  dedupToolResult(dedupMap, "read_file", "content A", 1);
  dedupToolResult(dedupMap, "read_file", "content B", 2);
  EXPECT_EQ(dedupMap.size(), 2u);

  auto outA = dedupToolResult(dedupMap, "read_file", "content A", 3);
  EXPECT_EQ(outA, "↪ identical to event #1");

  auto outB = dedupToolResult(dedupMap, "read_file", "content B", 4);
  EXPECT_EQ(outB, "↪ identical to event #2");
}

TEST(ToolOutputDedup, DifferentToolsSameContentNotDeduped) {
  std::unordered_map<uint64_t, DedupEntry> dedupMap;
  dedupToolResult(dedupMap, "read_file", "same content", 1);
  dedupToolResult(dedupMap, "grep", "same content", 2);
  EXPECT_EQ(dedupMap.size(), 2u);
}

TEST(ToolOutputDedup, FiftyIdenticalCallsCollapse) {
  std::unordered_map<uint64_t, DedupEntry> dedupMap;
  std::string result = dedupToolResult(dedupMap, "read_file", "file data", 0);
  EXPECT_EQ(result, "file data");

  for (uint64_t i = 1; i < 50; ++i) {
    auto ref = dedupToolResult(dedupMap, "read_file", "file data", i);
    EXPECT_EQ(ref, "↪ identical to event #0");
  }
  EXPECT_EQ(dedupMap.size(), 1u);
}
