#include "providers/AntigravityProvider.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

using firmius::provider::AntigravityProvider;
using firmius::shared::AgentMetrics;
using firmius::shared::StreamEvent;
using firmius::shared::TextChunk;
using firmius::shared::ThinkingChunk;
using firmius::shared::ToolCallChunk;

namespace {

class ScopedHomeOverride {
public:
  explicit ScopedHomeOverride(const std::filesystem::path &home)
      : hadHome_(std::getenv("HOME") != nullptr),
        originalHome_(hadHome_ ? std::getenv("HOME") : "") {
    setenv("HOME", home.c_str(), 1);
  }

  ~ScopedHomeOverride() {
    if (hadHome_) {
      setenv("HOME", originalHome_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
  }

private:
  bool hadHome_ = false;
  std::string originalHome_;
};

std::vector<StreamEvent> collectEvents(AntigravityProvider &provider,
                                       const std::vector<std::string> &lines) {
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> onEvent =
      [&](const StreamEvent &event) { events.push_back(event); };
  AntigravityProvider::StreamContext ctx;
  ctx.provider = &provider;
  ctx.onEvent = &onEvent;
  for (const auto &line : lines) {
    provider.processSSELine(line, ctx);
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

TEST(AntigravityProvider, AvailableAccountUsesHighestQuotaWithinModelBucket) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_antigravity_quota_pick_home";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  out << R"({"antigravity":[)"
      << R"({"identifier":"a@example.com","refreshToken":"r1","accessToken":"a1","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:claude-sonnet-4-6":"0.2","quota:gemini-3-flash":"0.9"}},)"
      << R"({"identifier":"b@example.com","refreshToken":"r2","accessToken":"a2","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:claude-sonnet-4-6":"0.8","quota:gemini-3-flash":"0.1"}},)"
      << R"({"identifier":"c@example.com","refreshToken":"r3","accessToken":"a3","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:gemini-3.1-pro-high":"0.7"}}]})";
  out.close();

  AntigravityProvider provider;

  auto claude = provider.getAvailableAccount(std::string("claude-sonnet-4-6"));
  ASSERT_TRUE(claude.has_value());
  EXPECT_EQ((*claude)->getIdentifier(), "b@example.com");

  auto flash = provider.getAvailableAccount(std::string("gemini-3-flash"));
  ASSERT_TRUE(flash.has_value());
  EXPECT_EQ((*flash)->getIdentifier(), "a@example.com");

  auto pro = provider.getAvailableAccount(std::string("gemini-3.1-pro"));
  ASSERT_TRUE(pro.has_value());
  EXPECT_EQ((*pro)->getIdentifier(), "c@example.com");
}

TEST(AntigravityProvider, AvailableAccountRequiresPositiveQuotaInRequestedBucket) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_antigravity_quota_none_home";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  const auto futureSeconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
      86400;
  const std::filesystem::path oauthPath = tempHome / ".firmius" / "oauth.json";
  std::ofstream out(oauthPath);
  out << R"({"antigravity":[)"
      << R"({"identifier":"a@example.com","refreshToken":"r1","accessToken":"a1","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:gemini-3-flash":"0.4"}},)"
      << R"({"identifier":"b@example.com","refreshToken":"r2","accessToken":"a2","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:claude-sonnet-4-6":"0"}}]})";
  out.close();

  AntigravityProvider provider;
  auto claude = provider.getAvailableAccount(std::string("claude-sonnet-4-6"));
  EXPECT_FALSE(claude.has_value());
}
