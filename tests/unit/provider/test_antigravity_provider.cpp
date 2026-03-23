#include "providers/AntigravityProvider.hpp"

#include <gtest/gtest.h>

#include <vector>

using firmius::provider::AntigravityProvider;
using firmius::shared::AgentMetrics;
using firmius::shared::StreamEvent;
using firmius::shared::TextChunk;
using firmius::shared::ThinkingChunk;
using firmius::shared::ToolCallChunk;

namespace {

std::vector<StreamEvent> collectEvents(AntigravityProvider &provider,
                                       const std::vector<std::string> &lines) {
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> onEvent =
      [&](const StreamEvent &event) { events.push_back(event); };
  for (const auto &line : lines) {
    provider.processSSELine(line, onEvent);
  }
  return events;
}

} // namespace

TEST(AntigravityProvider,
     ThinkingFieldWithoutExplicitTypeStillEmitsThinkingChunk) {
  AntigravityProvider provider;
  const auto events = collectEvents(
      provider,
      {R"(data: {"response":{"usageMetadata":{"thoughtsTokenCount":11},"candidates":[{"content":{"parts":[{"thinking":"Plan next step.","thought_signature":"sig-1"}]}}]}})"});

  ASSERT_EQ(events.size(), 2u);
  const auto *metrics = std::get_if<AgentMetrics>(&events[0]);
  ASSERT_NE(metrics, nullptr);
  EXPECT_EQ(metrics->tokens.reasoning, 11u);

  const auto *thinking = std::get_if<ThinkingChunk>(&events[1]);
  ASSERT_NE(thinking, nullptr);
  EXPECT_EQ(thinking->delta, "Plan next step.");
  EXPECT_EQ(thinking->signature, "sig-1");
}

TEST(AntigravityProvider,
     StringToolArgsStayRawAndRepeatedNamesDoNotDuplicateAcrossChunks) {
  AntigravityProvider provider;
  const auto events = collectEvents(
      provider,
      {
          R"(data: {"response":{"candidates":[{"content":{"parts":[{"functionCall":{"id":"call-1","name":"file_read","args":"{"}}]}}]}})",
          R"(data: {"response":{"candidates":[{"content":{"parts":[{"functionCall":{"id":"call-1","name":"file_read","args":"\"path\":\"src/main.cpp\"}"}}]}}]}})",
      });

  ASSERT_EQ(events.size(), 2u);

  const auto *first = std::get_if<ToolCallChunk>(&events[0]);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->id, "call-1");
  EXPECT_EQ(first->index, 0u);
  EXPECT_EQ(first->nameDelta, "file_read");
  EXPECT_EQ(first->argsDelta, "{");

  const auto *second = std::get_if<ToolCallChunk>(&events[1]);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->id, "call-1");
  EXPECT_EQ(second->index, 0u);
  EXPECT_TRUE(second->nameDelta.empty());
  EXPECT_EQ(second->argsDelta, "\"path\":\"src/main.cpp\"}");
}

TEST(AntigravityProvider, DuplicateFunctionCallSnapshotsAreSuppressed) {
  AntigravityProvider provider;
  const auto events = collectEvents(
      provider,
      {
          R"(data: {"response":{"candidates":[{"content":{"parts":[{"functionCall":{"id":"call-1","name":"file_read","args":{"path":"src/main.cpp"}}}]}}]}})",
          R"(data: {"response":{"candidates":[{"content":{"parts":[{"functionCall":{"id":"call-1","name":"file_read","args":{"path":"src/main.cpp"}}}]}}]}})",
      });

  ASSERT_EQ(events.size(), 1u);
  const auto *chunk = std::get_if<ToolCallChunk>(&events[0]);
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->nameDelta, "file_read");
  EXPECT_EQ(chunk->argsDelta, R"({"path":"src/main.cpp"})");
}
