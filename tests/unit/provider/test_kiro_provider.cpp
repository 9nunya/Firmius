#include "providers/KiroProvider.hpp"

#include "Context.hpp"
#include "Events.hpp"
#include "Message.hpp"

#include <gtest/gtest.h>

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

} // namespace
