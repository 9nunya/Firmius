#include "providers/KiroProvider.hpp"

#include "Context.hpp"
#include "Events.hpp"
#include "Message.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

using firmius::provider::KiroProvider;
using firmius::shared::AgentHistory;
using firmius::shared::AgentTurn;
using firmius::shared::Message;
using firmius::shared::OAuthAccount;
using firmius::shared::Role;
using firmius::shared::StreamEvent;
using firmius::shared::TextChunk;
using firmius::shared::ThinkingChunk;
using firmius::shared::ToolCall;
using firmius::shared::ToolCallChunk;
using firmius::shared::ToolCallContent;
using firmius::shared::TextContent;
using firmius::provider::ProviderOptions;

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

class KiroProviderTest : public ::testing::Test {
protected:
  void SetUp() override {
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_kiro_provider_" +
                 std::to_string(
                     ::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(testHome_);
    std::filesystem::create_directories(testHome_ / ".firmius");
    homeOverride_ = std::make_unique<ScopedHomeOverride>(testHome_);
  }

  void TearDown() override {
    homeOverride_.reset();
    std::filesystem::remove_all(testHome_);
  }

  std::filesystem::path testHome_;
  std::unique_ptr<ScopedHomeOverride> homeOverride_;
};

TEST_F(KiroProviderTest, BuildRequestSerializesAssistantToolInputsAsJsonObjects) {
  KiroProvider provider;

  AgentHistory history;
  AgentTurn turn;

  Message assistant;
  assistant.role = Role::Assistant;
  assistant.content.push_back(TextContent{"Running tool."});
  assistant.content.push_back(
      ToolCallContent{"call_1", "read_file", R"({"path":"README.md"})"});
  turn.messages.push_back(std::move(assistant));

  Message user;
  user.role = Role::User;
  user.content.push_back(TextContent{"What happened?"});
  turn.messages.push_back(std::move(user));
  history.turns.push_back(std::move(turn));

  OAuthAccount acc;
  acc.accessToken = "access";
  acc.refreshToken = "refresh";
  acc.metadata["region"] = "us-east-1";

  ProviderOptions opts;
  opts.modelId = "qwen3-coder-next";

  const std::string request = provider.buildCodeWhispererRequestForTest(
      history, opts.modelId, acc, opts);

  EXPECT_NE(request.find(R"("toolUses":[{"toolUseId":"call_1","name":"read_file","input":{"path":"README.md"}}])"),
            std::string::npos);
}

TEST_F(KiroProviderTest, StreamParserKeepsToolInputOnSameToolCallId) {
  KiroProvider provider;
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> handler =
      [&](const StreamEvent &ev) { events.push_back(ev); };

  KiroProvider::StreamContext ctx;
  ctx.provider = &provider;
  ctx.onEvent = &handler;

  const std::string firstChunk =
      "{\"name\":\"read_file\",\"toolUseId\":\"tool_1\",\"input\":\"{\"}";
  const std::string secondChunk =
      "{\"input\":\"\\\"path\\\":\\\"README.md\\\"}\"}"
      "{\"stop\":true}";

  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(firstChunk.data()),
                                        1, firstChunk.size(), &ctx);
  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(secondChunk.data()),
                                        1, secondChunk.size(), &ctx);

  ASSERT_EQ(events.size(), 3u);

  const auto *toolStart = std::get_if<ToolCallChunk>(&events[0]);
  ASSERT_NE(toolStart, nullptr);

  const auto *toolInput = std::get_if<ToolCallChunk>(&events[1]);
  ASSERT_NE(toolInput, nullptr);

  const auto *finalCall = std::get_if<ToolCall>(&events[2]);
  ASSERT_NE(finalCall, nullptr);
  EXPECT_EQ(toolStart->id, "tool_1");
  EXPECT_EQ(toolStart->nameDelta, "read_file");
  EXPECT_EQ(toolStart->argsDelta, "{");

  EXPECT_EQ(toolInput->argsDelta, R"("path":"README.md"})");
  EXPECT_EQ(finalCall->id, "tool_1");
  EXPECT_EQ(finalCall->name, "read_file");
  EXPECT_EQ(finalCall->args, R"({"path":"README.md"})");
}

TEST_F(KiroProviderTest, StreamParserExtractsThinkingTagsIntoReasoningChunks) {
  KiroProvider provider;
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> handler =
      [&](const StreamEvent &ev) { events.push_back(ev); };

  KiroProvider::StreamContext ctx;
  ctx.provider = &provider;
  ctx.onEvent = &handler;

  const std::string chunk =
      "{\"content\":\"<thinking>plan</thinking>answer\"}";

  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(chunk.data()), 1,
                                        chunk.size(), &ctx);

  ASSERT_GE(events.size(), 1u);
  const auto *thinking = std::get_if<ThinkingChunk>(&events[0]);
  ASSERT_NE(thinking, nullptr);
  EXPECT_EQ(thinking->delta, "plan");
}

TEST_F(KiroProviderTest, StreamParserExtractsThinkingTagsIntoReasoningChunks_Uppercase) {
  KiroProvider provider;
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> handler =
      [&](const StreamEvent &ev) { events.push_back(ev); };

  KiroProvider::StreamContext ctx;
  ctx.provider = &provider;
  ctx.onEvent = &handler;

  const std::string chunk =
      "{\"content\":\"<THINKING>plan</THINKING>answer\"}";

  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(chunk.data()), 1,
                                        chunk.size(), &ctx);

  ASSERT_GE(events.size(), 1u);
  const auto *thinking = std::get_if<ThinkingChunk>(&events[0]);
  ASSERT_NE(thinking, nullptr);
  EXPECT_EQ(thinking->delta, "plan");
}

TEST_F(KiroProviderTest, StreamParserEmitsThinkingDeltaFieldWhenPresent) {
  KiroProvider provider;
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> handler =
      [&](const StreamEvent &ev) { events.push_back(ev); };

  KiroProvider::StreamContext ctx;
  ctx.provider = &provider;
  ctx.onEvent = &handler;

  const std::string chunk = "{\"thinking\":\"hidden\"}";
  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(chunk.data()), 1,
                                        chunk.size(), &ctx);

  ASSERT_EQ(events.size(), 1u);
  const auto *thinking = std::get_if<ThinkingChunk>(&events[0]);
  ASSERT_NE(thinking, nullptr);
  EXPECT_EQ(thinking->delta, "hidden");
}

TEST_F(KiroProviderTest, LegacyEncodedRefreshTokenIsMigratedWithoutCorruption) {
  const auto oauthPath = testHome_ / ".firmius" / "oauth.json";
  std::ofstream oauth(oauthPath);
  oauth << R"({
    "kiro": [{
      "identifier": "kiro-deadbeef",
      "refreshToken": "legacy-refresh|client-id|client-secret|idc",
      "accessToken": "access",
      "tokenExpiration": 9999999999,
      "lastQuotaRefresh": 0,
      "metadata": {
        "email": "user@example.com",
        "profileArn": "arn:aws:codewhisperer:us-east-1:123456789012:profile/ABC"
      }
    }]
  })";
  oauth.close();

  KiroProvider provider;
  const auto accounts = provider.getAccounts();
  ASSERT_EQ(accounts.size(), 1u);
  EXPECT_EQ(accounts.front().refreshToken, "legacy-refresh");
  EXPECT_EQ(accounts.front().metadata.at("authMethod"), "idc");
  EXPECT_EQ(accounts.front().metadata.at("clientId"), "client-id");
  EXPECT_EQ(accounts.front().metadata.at("clientSecret"), "client-secret");
  EXPECT_TRUE(accounts.front().identifier.starts_with("kiro-"));
  EXPECT_NE(accounts.front().identifier, "kiro-deadbeef");
}

TEST_F(KiroProviderTest, EmptyRefreshQuotasDoesNotCreateOrWipeKiroSection) {
  KiroProvider provider;
  provider.refreshQuotas();

  const auto oauthPath = testHome_ / ".firmius" / "oauth.json";
  if (std::filesystem::exists(oauthPath)) {
    const auto content = std::filesystem::file_size(oauthPath) == 0
                             ? std::string{}
                             : [&]() {
                                 std::ifstream in(oauthPath);
                                 return std::string(
                                     (std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
                               }();
    EXPECT_TRUE(content.empty() || content == "{}");
  } else {
    SUCCEED();
  }
}

// ===========================================================================
// New model catalog: every Kiro-supported model must be present and tagged.
// ===========================================================================

TEST_F(KiroProviderTest, ListModelsContainsCurrentKiroCatalog) {
  KiroProvider provider;
  const auto models = provider.listModels();

  std::vector<std::string> ids;
  ids.reserve(models.size());
  for (const auto &m : models) {
    ids.push_back(m.id);
  }

  // Snapshot of the Kiro CLI /model picker as of May 2026. We assert each one
  // is present so future catalog regressions break the build instead of
  // silently dropping a model from /model.
  for (const char *expected : {
           "auto", "claude-opus-4.7", "claude-opus-4.6", "claude-opus-4.5",
           "claude-sonnet-4.6", "claude-sonnet-4.5", "claude-sonnet-4",
           "claude-haiku-4.5", "deepseek-3.2", "minimax-m2.5", "minimax-m2.1",
           "glm-5", "qwen3-coder-next"}) {
    EXPECT_NE(std::find(ids.begin(), ids.end(), expected), ids.end())
        << "Missing model in catalog: " << expected;
  }
}

TEST_F(KiroProviderTest, ModelMinimumTierMatchesKiroPricing) {
  using firmius::provider::KiroTier;
  using firmius::provider::KiroProvider;
  // Free-tier-eligible per https://kiro.dev/docs/models/.
  for (const char *id : {"auto", "claude-sonnet-4.5", "claude-sonnet-4",
                         "deepseek-3.2", "minimax-m2.5", "minimax-m2.1",
                         "glm-5", "qwen3-coder-next"}) {
    EXPECT_EQ(KiroProvider::modelMinimumTier(id), KiroTier::Free)
        << id << " should be available on Free";
  }
  // Paid-only per the same matrix.
  for (const char *id : {"claude-opus-4.7", "claude-opus-4.6", "claude-opus-4.5",
                         "claude-sonnet-4.6", "claude-haiku-4.5"}) {
    EXPECT_EQ(KiroProvider::modelMinimumTier(id), KiroTier::Pro)
        << id << " should require a paid plan";
  }
}

// ===========================================================================
// Tier resolution from the live /getUsageLimits response.
// ===========================================================================

TEST_F(KiroProviderTest, ParseUsageLimitsRecognisesExplicitSubscriptionType) {
  OAuthAccount acc;
  const std::string body = R"({
    "subscriptionType": "PRO_PLUS",
    "userInfo": {"email": "user@example.com"},
    "usageBreakdownList": [
      {"currentUsage": 250, "usageLimit": 2000}
    ]
  })";
  ASSERT_TRUE(KiroProvider::applyUsageLimitsResponseForTest(acc, body));
  EXPECT_EQ(acc.metadata.at("kiroTier"), "pro_plus");
  EXPECT_EQ(acc.metadata.at("plan_tier"), "Kiro Pro+");
  EXPECT_EQ(acc.metadata.at("usedCount"), "250");
  EXPECT_EQ(acc.metadata.at("limitCount"), "2000");
  EXPECT_EQ(acc.metadata.at("email"), "user@example.com");
}

TEST_F(KiroProviderTest, ParseUsageLimitsFallsBackToStructureForFreeTier) {
  // Builder ID Free accounts only get a freeTrialInfo bucket and no named
  // subscription field. The parser must classify this as Free without
  // guessing.
  OAuthAccount acc;
  const std::string body = R"({
    "userInfo": {"email": "free@example.com"},
    "usageBreakdownList": [
      {"freeTrialInfo": {"currentUsage": 17, "usageLimit": 50}}
    ]
  })";
  ASSERT_TRUE(KiroProvider::applyUsageLimitsResponseForTest(acc, body));
  EXPECT_EQ(acc.metadata.at("kiroTier"), "free");
  EXPECT_EQ(acc.metadata.at("plan_tier"), "Kiro Free");
}

TEST_F(KiroProviderTest, ParseUsageLimitsFallsBackToStructureForPower) {
  // No subscriptionType, but a paid bucket with a 10000-credit Power-sized
  // limit — the structural fallback must classify this as Power.
  OAuthAccount acc;
  const std::string body = R"({
    "userInfo": {"email": "power@example.com"},
    "usageBreakdownList": [
      {"currentUsage": 1234, "usageLimit": 10000}
    ]
  })";
  ASSERT_TRUE(KiroProvider::applyUsageLimitsResponseForTest(acc, body));
  EXPECT_EQ(acc.metadata.at("kiroTier"), "power");
  EXPECT_EQ(acc.metadata.at("plan_tier"), "Kiro Power");
}

TEST_F(KiroProviderTest, AccountTierGatesPaidOnlyModels) {
  using firmius::provider::KiroProvider;
  using firmius::provider::KiroTier;

  OAuthAccount free;
  free.metadata["kiroTier"] = "free";
  EXPECT_TRUE(KiroProvider::accountTierMeetsModelMinimum(
      KiroProvider::accountTier(free), "claude-sonnet-4.5"));
  EXPECT_FALSE(KiroProvider::accountTierMeetsModelMinimum(
      KiroProvider::accountTier(free), "claude-opus-4.7"));

  OAuthAccount pro;
  pro.metadata["kiroTier"] = "pro";
  EXPECT_TRUE(KiroProvider::accountTierMeetsModelMinimum(
      KiroProvider::accountTier(pro), "claude-opus-4.7"));
  EXPECT_TRUE(KiroProvider::accountTierMeetsModelMinimum(
      KiroProvider::accountTier(pro), "glm-5"));

  // Unknown tier must not lock the user out before the first usage-limits
  // roundtrip lands.
  OAuthAccount unknown;
  EXPECT_TRUE(KiroProvider::accountTierMeetsModelMinimum(
      KiroProvider::accountTier(unknown), "claude-opus-4.7"));
}

// ===========================================================================
// SSE parser: Bedrock content_block_* and OpenAI choices/delta shapes used by
// GLM-5, MiniMax, DeepSeek, and Qwen via the Kiro Q endpoint.
// ===========================================================================

TEST_F(KiroProviderTest, StreamParserHandlesBedrockThinkingDelta) {
  KiroProvider provider;
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> handler =
      [&](const StreamEvent &ev) { events.push_back(ev); };

  KiroProvider::StreamContext ctx;
  ctx.provider = &provider;
  ctx.onEvent = &handler;

  const std::string chunk =
      R"({"type":"content_block_delta","delta":{"type":"thinking_delta","thinking":"weighing options"}})";
  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(chunk.data()), 1,
                                        chunk.size(), &ctx);

  ASSERT_GE(events.size(), 1u);
  const auto *thinking = std::get_if<ThinkingChunk>(&events[0]);
  ASSERT_NE(thinking, nullptr);
  EXPECT_EQ(thinking->delta, "weighing options");
}

TEST_F(KiroProviderTest, StreamParserHandlesBedrockTextDelta) {
  KiroProvider provider;
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> handler =
      [&](const StreamEvent &ev) { events.push_back(ev); };

  KiroProvider::StreamContext ctx;
  ctx.provider = &provider;
  ctx.onEvent = &handler;
  ctx.thinkingExtracted = true; // No <thinking> wrapping for this dialect.

  const std::string chunk =
      R"({"type":"content_block_delta","delta":{"type":"text_delta","text":"hello"}})";
  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(chunk.data()), 1,
                                        chunk.size(), &ctx);

  bool sawText = false;
  for (const auto &ev : events) {
    if (const auto *t = std::get_if<TextChunk>(&ev); t && t->delta == "hello") {
      sawText = true;
    }
  }
  EXPECT_TRUE(sawText);
}

TEST_F(KiroProviderTest, StreamParserHandlesBedrockToolUseSequence) {
  KiroProvider provider;
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> handler =
      [&](const StreamEvent &ev) { events.push_back(ev); };

  KiroProvider::StreamContext ctx;
  ctx.provider = &provider;
  ctx.onEvent = &handler;

  const std::string startChunk =
      R"({"type":"content_block_start","content_block":{"type":"tool_use","id":"tu_42","name":"read_file","input":{}}})";
  const std::string deltaChunk =
      R"({"type":"content_block_delta","delta":{"type":"input_json_delta","partial_json":"{\"path\":\"x\"}"}})";
  const std::string stopChunk = R"({"type":"content_block_stop"})";

  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(startChunk.data()),
                                        1, startChunk.size(), &ctx);
  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(deltaChunk.data()),
                                        1, deltaChunk.size(), &ctx);
  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(stopChunk.data()), 1,
                                        stopChunk.size(), &ctx);

  bool sawStart = false;
  bool sawArgsDelta = false;
  const ToolCall *finalCall = nullptr;
  for (const auto &ev : events) {
    if (const auto *chunk = std::get_if<ToolCallChunk>(&ev)) {
      if (chunk->id == "tu_42" && chunk->nameDelta == "read_file") {
        sawStart = true;
      }
      if (chunk->id == "tu_42" && chunk->argsDelta == R"({"path":"x"})") {
        sawArgsDelta = true;
      }
    }
    if (const auto *call = std::get_if<ToolCall>(&ev)) {
      finalCall = call;
    }
  }
  EXPECT_TRUE(sawStart);
  EXPECT_TRUE(sawArgsDelta);
  ASSERT_NE(finalCall, nullptr);
  EXPECT_EQ(finalCall->id, "tu_42");
  EXPECT_EQ(finalCall->name, "read_file");
  EXPECT_EQ(finalCall->args, R"({"path":"x"})");
}

TEST_F(KiroProviderTest, StreamParserHandlesOpenAIReasoningContent) {
  // DeepSeek-V3 / Qwen3 surface their thinking via OpenAI-style
  // delta.reasoning_content chunks when routed through Kiro.
  KiroProvider provider;
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> handler =
      [&](const StreamEvent &ev) { events.push_back(ev); };

  KiroProvider::StreamContext ctx;
  ctx.provider = &provider;
  ctx.onEvent = &handler;

  const std::string chunk =
      R"({"choices":[{"delta":{"reasoning_content":"step 1","content":"answer"}}]})";
  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(chunk.data()), 1,
                                        chunk.size(), &ctx);

  bool sawThinking = false;
  bool sawText = false;
  for (const auto &ev : events) {
    if (const auto *t = std::get_if<ThinkingChunk>(&ev); t && t->delta == "step 1") {
      sawThinking = true;
    }
  }
  // Trigger any tail-flushed text by consuming a stop event.
  const std::string stop = R"({"choices":[{"delta":{},"finish_reason":"stop"}]})";
  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(stop.data()), 1,
                                        stop.size(), &ctx);
  for (const auto &ev : events) {
    if (const auto *t = std::get_if<TextChunk>(&ev);
        t && t->delta.find("answer") != std::string::npos) {
      sawText = true;
    }
  }
  EXPECT_TRUE(sawThinking);
  EXPECT_TRUE(sawText);
}

TEST_F(KiroProviderTest, StreamParserHandlesOpenAIToolCalls) {
  KiroProvider provider;
  std::vector<StreamEvent> events;
  std::function<void(const StreamEvent &)> handler =
      [&](const StreamEvent &ev) { events.push_back(ev); };

  KiroProvider::StreamContext ctx;
  ctx.provider = &provider;
  ctx.onEvent = &handler;

  const std::string startChunk =
      R"({"choices":[{"delta":{"tool_calls":[{"id":"call_99","function":{"name":"grep","arguments":"{\"pat\":\"foo\""}}]}}]})";
  const std::string deltaChunk =
      R"({"choices":[{"delta":{"tool_calls":[{"id":"call_99","function":{"arguments":"}"}}]}}]})";
  const std::string finishChunk =
      R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})";

  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(startChunk.data()),
                                        1, startChunk.size(), &ctx);
  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(deltaChunk.data()),
                                        1, deltaChunk.size(), &ctx);
  KiroProvider::sseWriteCallbackForTest(const_cast<char *>(finishChunk.data()),
                                        1, finishChunk.size(), &ctx);

  const ToolCall *finalCall = nullptr;
  for (const auto &ev : events) {
    if (const auto *call = std::get_if<ToolCall>(&ev)) {
      finalCall = call;
    }
  }
  ASSERT_NE(finalCall, nullptr);
  EXPECT_EQ(finalCall->id, "call_99");
  EXPECT_EQ(finalCall->name, "grep");
  EXPECT_EQ(finalCall->args, R"({"pat":"foo"})");
}

} // namespace
