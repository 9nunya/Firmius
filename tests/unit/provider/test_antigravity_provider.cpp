#include "providers/AntigravityProvider.hpp"
#include "providers/AntigravityProtocol.hpp"
#include "providers/LLMSearchProvider.hpp"
#include "providers/LLMSearchProviderRegistry.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using firmius::provider::AntigravityProvider;
using firmius::provider::AntigravityProtocol;
using firmius::provider::LLMSearchProvider;
using firmius::provider::LLMSearchProviderRegistry;
using firmius::provider::ProviderOptions;
using firmius::shared::AgentMetrics;
using firmius::shared::AgentHistory;
using firmius::shared::AgentTurn;
using firmius::shared::Message;
using firmius::shared::Role;
using firmius::shared::StreamEvent;
using firmius::shared::TextChunk;
using firmius::shared::ThinkingChunk;
using firmius::shared::ToolCall;
using firmius::shared::ToolCallChunk;
using firmius::shared::ToolCallContent;
using firmius::shared::ToolResultContent;

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

std::string firstFunctionResponseName(const std::string &body) {
  rapidjson::Document doc;
  doc.Parse(body.c_str());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("request") ||
      !doc["request"].IsObject()) {
    return "";
  }
  const auto &request = doc["request"];
  if (!request.HasMember("contents") || !request["contents"].IsArray()) {
    return "";
  }
  for (const auto &turn : request["contents"].GetArray()) {
    if (!turn.IsObject() || !turn.HasMember("parts") || !turn["parts"].IsArray()) {
      continue;
    }
    for (const auto &part : turn["parts"].GetArray()) {
      if (!part.IsObject() || !part.HasMember("functionResponse") ||
          !part["functionResponse"].IsObject()) {
        continue;
      }
      const auto &response = part["functionResponse"];
      if (response.HasMember("name") && response["name"].IsString()) {
        return response["name"].GetString();
      }
    }
  }
  return "";
}

class BlockingSearchProvider : public LLMSearchProvider {
public:
  explicit BlockingSearchProvider(std::shared_future<void> release)
      : release_(std::move(release)) {}

  std::string name() const override { return "blocking-search-test"; }

  bool isAvailable() const override {
    entered_.store(true, std::memory_order_relaxed);
    while (release_.wait_for(std::chrono::milliseconds(5)) !=
           std::future_status::ready) {
    }
    return false;
  }

  firmius::provider::SearchResult
  search(const std::string &, const std::vector<std::string> &) override {
    return {};
  }

  bool entered() const { return entered_.load(std::memory_order_relaxed); }

private:
  std::shared_future<void> release_;
  mutable std::atomic<bool> entered_{false};
};

class ReadySearchProvider : public LLMSearchProvider {
public:
  std::string name() const override { return "ready-search-test"; }

  bool isAvailable() const override { return true; }

  firmius::provider::SearchResult
  search(const std::string &, const std::vector<std::string> &) override {
    return {};
  }
};

class CountingAntigravityProvider : public AntigravityProvider {
public:
  bool configured = false;
  mutable std::atomic<int> isConfiguredCalls{0};
  mutable std::atomic<int> availableAccountCalls{0};

  bool isConfigured() const override {
    ++isConfiguredCalls;
    return configured;
  }

  std::optional<firmius::shared::OAuthAccount>
  getAvailableAccount(const std::optional<std::string> &modelId =
                          std::nullopt) override {
    (void)modelId;
    ++availableAccountCalls;
    return std::nullopt;
  }
};

} // namespace

TEST(SearchProviderRegistry, AvailabilityChecksDoNotHoldRegistryMutex) {
  auto &registry = LLMSearchProviderRegistry::instance();
  registry.unregisterProvider("blocking-search-test");
  registry.unregisterProvider("ready-search-test");

  std::promise<void> releasePromise;
  auto release = releasePromise.get_future().share();
  auto blocking = std::make_shared<BlockingSearchProvider>(release);
  auto ready = std::make_shared<ReadySearchProvider>();
  registry.registerProvider(blocking);
  registry.registerProvider(ready);

  auto lookup = std::async(std::launch::async, [&registry]() {
    return registry.getFirstAvailable();
  });

  for (int i = 0; i < 50 && !blocking->entered(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(blocking->entered());

  auto listNames = std::async(std::launch::async, [&registry]() {
    return registry.listProviderNames();
  });
  EXPECT_EQ(listNames.wait_for(std::chrono::milliseconds(100)),
            std::future_status::ready);

  releasePromise.set_value();

  auto first = lookup.get();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->get().name(), "ready-search-test");

  registry.unregisterProvider("blocking-search-test");
  registry.unregisterProvider("ready-search-test");
}

TEST(GoogleSearchProvider, AvailabilityCheckDoesNotProbeAccounts) {
  const auto tempHome =
      std::filesystem::temp_directory_path() /
      "firmius_google_search_is_available_home";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome / ".firmius");
  ScopedHomeOverride scopedHome(tempHome);

  auto &registry = LLMSearchProviderRegistry::instance();
  registry.unregisterProvider("google-search");

  CountingAntigravityProvider provider;
  provider.configured = true;

  auto first = registry.getFirstAvailable();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->get().name(), "google-search");
  EXPECT_GT(provider.isConfiguredCalls.load(), 0);
  EXPECT_EQ(provider.availableAccountCalls.load(), 0);

  registry.unregisterProvider("google-search");
}

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

  ASSERT_GE(events.size(), 2u);

  const auto *first = std::get_if<ToolCallChunk>(&events[0]);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->id, "call-1");
  EXPECT_EQ(first->index, 0u);
  EXPECT_EQ(first->nameDelta, "file_read");
  EXPECT_EQ(first->argsDelta, "{");

  const auto *finalCall = std::get_if<ToolCall>(&events.back());
  ASSERT_NE(finalCall, nullptr);
  EXPECT_EQ(finalCall->id, "call-1");
  EXPECT_EQ(finalCall->index, 0u);
  EXPECT_EQ(finalCall->name, "file_read");
  EXPECT_EQ(finalCall->args, R"({"path":"src/main.cpp"})");
}

TEST(AntigravityProvider, ValidFunctionCallSnapshotEmitsFinalToolCallOnce) {
  AntigravityProvider provider;
  const auto events = collectEvents(
      provider,
      {
          R"(data: {"response":{"candidates":[{"content":{"parts":[{"functionCall":{"id":"call-1","name":"file_read","args":{"path":"src/main.cpp"}}}]}}]}})",
          R"(data: {"response":{"candidates":[{"content":{"parts":[{"functionCall":{"id":"call-1","name":"file_read","args":{"path":"src/main.cpp"}}}]}}]}})",
      });

  ASSERT_EQ(events.size(), 2u);
  const auto *chunk = std::get_if<ToolCallChunk>(&events.front());
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->id, "call-1");
  EXPECT_EQ(chunk->nameDelta, "file_read");
  EXPECT_EQ(chunk->argsDelta, R"({"path":"src/main.cpp"})");

  const auto *finalCall = std::get_if<ToolCall>(&events.back());
  ASSERT_NE(finalCall, nullptr);
  EXPECT_EQ(finalCall->name, "file_read");
  EXPECT_EQ(finalCall->args, R"({"path":"src/main.cpp"})");
}

TEST(AntigravityProvider, DuplicateFunctionCallSnapshotsAreSuppressed) {
  AntigravityProvider provider;
  const auto events = collectEvents(
      provider,
      {
          R"(data: {"response":{"candidates":[{"content":{"parts":[{"functionCall":{"id":"call-1","name":"file_read","args":{"path":"src/main.cpp"}}}]}}]}})",
          R"(data: {"response":{"candidates":[{"content":{"parts":[{"functionCall":{"id":"call-1","name":"file_read","args":{"path":"src/main.cpp"}}}]}}]}})",
      });

  ASSERT_EQ(events.size(), 2u);
  const auto *chunk = std::get_if<ToolCallChunk>(&events[0]);
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->nameDelta, "file_read");
  EXPECT_EQ(chunk->argsDelta, R"({"path":"src/main.cpp"})");

  const auto *finalCall = std::get_if<ToolCall>(&events[1]);
  ASSERT_NE(finalCall, nullptr);
  EXPECT_EQ(finalCall->name, "file_read");
  EXPECT_EQ(finalCall->args, R"({"path":"src/main.cpp"})");
}

TEST(AntigravityProvider,
     FunctionResponseNameFallsBackToPriorToolCallHistoryWhenIdIsMissing) {
  AgentHistory history;
  AgentTurn callTurn;
  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content.push_back(
      ToolCallContent{"", "file_read", R"({"path":"src/main.cpp"})"});
  callTurn.messages.push_back(std::move(assistant));
  history.turns.push_back(std::move(callTurn));

  AgentTurn resultTurn;
  Message toolResult;
  toolResult.role = Role::ToolResult;
  toolResult.content.push_back(
      ToolResultContent{"", R"({"content":"ok"})", true, "", ""});
  resultTurn.messages.push_back(std::move(toolResult));
  history.turns.push_back(std::move(resultTurn));

  ProviderOptions opts;
  AntigravityProtocol::RequestContext ctx{
      "antigravity-gemini-3-flash", "project", "session", "request"};
  const std::string body =
      AntigravityProtocol::prepareRequestBody(history, opts, ctx);

  EXPECT_EQ(firstFunctionResponseName(body), "file_read");
}

TEST(AntigravityProvider,
     FunctionResponseNameNeverFallsBackToEmptyString) {
  AgentHistory history;
  AgentTurn resultTurn;
  Message toolResult;
  toolResult.role = Role::ToolResult;
  toolResult.content.push_back(
      ToolResultContent{"unknown-id", R"({"content":"ok"})", true, "", ""});
  resultTurn.messages.push_back(std::move(toolResult));
  history.turns.push_back(std::move(resultTurn));

  ProviderOptions opts;
  AntigravityProtocol::RequestContext ctx{
      "antigravity-gemini-3-flash", "project", "session", "request"};
  const std::string body =
      AntigravityProtocol::prepareRequestBody(history, opts, ctx);

  EXPECT_EQ(firstFunctionResponseName(body), "tool_result");
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
  EXPECT_EQ(claude->getIdentifier(), "b@example.com");

  auto flash = provider.getAvailableAccount(std::string("gemini-3-flash"));
  ASSERT_TRUE(flash.has_value());
  EXPECT_EQ(flash->getIdentifier(), "a@example.com");

  auto pro = provider.getAvailableAccount(std::string("gemini-3.1-pro"));
  ASSERT_TRUE(pro.has_value());
  EXPECT_EQ(pro->getIdentifier(), "c@example.com");
}

TEST(AntigravityProvider, StaticModelsIncludeSupportedGeminiFlashVariantsOnly) {
  const auto models = AntigravityProvider::getStaticModels();

  EXPECT_EQ(models.count("gemini-3-flash"), 1u);
  EXPECT_EQ(models.count("gemini-2.5-flash"), 1u);
  EXPECT_EQ(models.count("gemini-2.5-flash-lite"), 1u);
  EXPECT_EQ(models.count("gemini-2.5-flash-thinking"), 1u);

  EXPECT_EQ(models.count("gemini-3-flash-agent"), 0u);
  EXPECT_EQ(models.count("gemini-3-pro"), 0u);

  EXPECT_EQ(models.at("gemini-3-flash").contextWindow, 1048576u);
  EXPECT_EQ(models.at("gemini-2.5-flash").contextWindow, 1048576u);
  EXPECT_EQ(models.at("gemini-2.5-flash-lite").contextWindow, 1048576u);
  EXPECT_EQ(models.at("gemini-2.5-flash-thinking").contextWindow, 1048576u);
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

TEST(AntigravityProvider, FlashAgentSelectsAgentQuotaWhenPresent) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_antigravity_flash_agent_home";
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
      << R"(,"metadata":{"quota:gemini-3-flash":"0.9","quota:gemini-3-flash-agent":"0.1"}},)"
      << R"({"identifier":"b@example.com","refreshToken":"r2","accessToken":"a2","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:gemini-3-flash":"0.2","quota:gemini-3-flash-agent":"0.8"}}]})";
  out.close();

  AntigravityProvider provider;
  auto flashAgent =
      provider.getAvailableAccount(std::string("gemini-3-flash-agent"));
  ASSERT_TRUE(flashAgent.has_value());
  EXPECT_EQ(flashAgent->getIdentifier(), "b@example.com");
}

TEST(AntigravityProvider, QuotasPreserveExactGeminiFamilies) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_antigravity_quota_display_home";
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
      << R"(,"metadata":{"quota:gemini-2.5-pro":"1.0","quota:gemini-3.1-pro-low":"0.0","quota:gemini-3-flash":"0.2"}}]})";
  out.close();

  AntigravityProvider provider;
  const auto quotas = provider.getAllQuotas();
  auto it = quotas.find("a@example.com");
  ASSERT_NE(it, quotas.end());

  std::map<std::string, float> bucketMap;
  for (const auto &bucket : it->second) {
    bucketMap[bucket.name] = bucket.remainingFraction;
  }

  EXPECT_EQ(bucketMap.count("gemini-2.5-pro"), 1u);
  EXPECT_EQ(bucketMap.count("gemini-3.1-pro"), 1u);
  EXPECT_EQ(bucketMap.count("gemini-3-flash"), 1u);
  EXPECT_EQ(bucketMap.count("gemini-pro"), 0u);
  EXPECT_FLOAT_EQ(bucketMap["gemini-2.5-pro"], 1.0f);
  EXPECT_FLOAT_EQ(bucketMap["gemini-3.1-pro"], 0.0f);
  EXPECT_FLOAT_EQ(bucketMap["gemini-3-flash"], 0.2f);
}

TEST(AntigravityProvider, TiedQuotaPrefersLastUsedAccount) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_antigravity_last_used_home";
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
      << R"(,"metadata":{"quota:gemini-3-flash":"0.2"}},)"
      << R"({"identifier":"b@example.com","refreshToken":"r2","accessToken":"a2","tokenExpiration":)"
      << futureSeconds
      << R"(,"metadata":{"quota:gemini-3-flash":"0.2"}}],)"
      << R"("lastUsedIndex_antigravity":1})";
  out.close();

  AntigravityProvider provider;
  auto flash = provider.getAvailableAccount(std::string("gemini-3-flash"));
  ASSERT_TRUE(flash.has_value());
  EXPECT_EQ(flash->getIdentifier(), "b@example.com");
}

TEST(AntigravityProvider, QuotasUseStreamQuotaExhaustionModelState) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_antigravity_claude_stream_quota_home";
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
      << R"(,"metadata":{"quota:claude-opus-4-6-thinking":"0","quota_reset:claude-opus-4-6-thinking":"2026-04-02T06:34:38Z","quota:claude-sonnet-4-6":"0.2","quota_reset:claude-sonnet-4-6":"2026-04-02T06:34:38Z"}}]})";
  out.close();

  AntigravityProvider provider;
  const auto quotas = provider.getAllQuotas();
  auto it = quotas.find("a@example.com");
  ASSERT_NE(it, quotas.end());

  std::map<std::string, float> bucketMap;
  for (const auto &bucket : it->second) {
    bucketMap[bucket.name] = bucket.remainingFraction;
  }

  EXPECT_EQ(bucketMap.count("claude-opus-4-6-thinking"), 1u);
  EXPECT_EQ(bucketMap.count("claude-sonnet-4-6"), 1u);
  EXPECT_FLOAT_EQ(bucketMap["claude-opus-4-6-thinking"], 0.0f);
  EXPECT_FLOAT_EQ(bucketMap["claude-sonnet-4-6"], 0.2f);
}

TEST(AntigravityProvider, QuotasDoNotRequireResetMetadataToRender) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_antigravity_quota_missing_reset_home";
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
      << R"(,"metadata":{"quota:claude-sonnet-4-6":"0.4","quota:gemini-3-flash":"bogus"}}]})";
  out.close();

  AntigravityProvider provider;
  std::map<std::string, std::vector<firmius::shared::QuotaBucket>> quotas;
  EXPECT_NO_THROW(quotas = provider.getAllQuotas());

  auto it = quotas.find("a@example.com");
  ASSERT_NE(it, quotas.end());
  ASSERT_EQ(it->second.size(), 1u);
  EXPECT_EQ(it->second.front().name, "claude-sonnet-4-6");
  EXPECT_FLOAT_EQ(it->second.front().remainingFraction, 0.4f);
  EXPECT_TRUE(it->second.front().resetTime.empty());
}
