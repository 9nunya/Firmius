#include "providers/QwenProvider.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

using firmius::provider::QwenProvider;
using firmius::provider::ProviderOptions;
using firmius::provider::ToolDefinition;
using firmius::shared::OAuthAccount;
using firmius::shared::AgentHistory;
using firmius::shared::AgentTurn;
using firmius::shared::ImageContent;
using firmius::shared::Message;
using firmius::shared::Role;
using firmius::shared::StreamError;
using firmius::shared::StreamRetrying;
using firmius::shared::TextContent;
using firmius::shared::TextChunk;
using firmius::shared::ThinkingChunk;
using firmius::shared::ToolCallContent;
using firmius::shared::ToolCallChunk;
using firmius::shared::ToolResultContent;

namespace {

Message makeMessage(Role role,
                    std::vector<firmius::shared::MessagePart> content,
                    std::uint64_t timestamp = 1) {
  Message message;
  message.role = role;
  message.content = std::move(content);
  message.timestamp = timestamp;
  return message;
}

AgentTurn makeTurn(const std::string &turnId, Message message) {
  AgentTurn turn;
  turn.turnId = turnId;
  turn.messages.push_back(std::move(message));
  return turn;
}

rapidjson::Document parsePayload(const std::string &body) {
  rapidjson::Document doc;
  doc.Parse(body.c_str());
  return doc;
}

} // namespace

TEST(QwenProvider, InvalidRequestErrorIsNonRetryable) {
  const std::string body =
      R"({"error":{"type":"invalid_request_error","message":"The \"function.arguments\" parameter of the code model must be in JSON format."}})";

  auto result = QwenProvider::classifyStreamFailure(400, body, {});

  EXPECT_EQ(result.kind, QwenProvider::StreamAttemptKind::NonRetryableRequest);
  EXPECT_NE(result.errorMessage.find("Raw provider body:"), std::string::npos);
  EXPECT_NE(result.errorMessage.find(body), std::string::npos);
}

TEST(QwenProvider, QuotaErrorRemainsSwitchable) {
  const std::string body =
      R"({"error":{"type":"invalid_request_error","message":"free allocated quota exceeded"}})";

  auto result = QwenProvider::classifyStreamFailure(
      400, body, {{"retry-after", "7"}});

  EXPECT_EQ(result.kind, QwenProvider::StreamAttemptKind::QuotaLimited);
  EXPECT_EQ(result.retryAfterMs, 7000);
}

TEST(QwenProvider, SingleAccountHasNoAlternativeSwitchTarget) {
  OAuthAccount account;
  account.identifier = "only@example.com";

  EXPECT_FALSE(
      QwenProvider::hasAlternativeAccount({account}, account.getIdentifier()));
}

TEST(QwenProvider, ComposeNoAlternateAccountErrorPreservesUnderlyingCause) {
  EXPECT_EQ(QwenProvider::composeNoAlternateAccountError("Request timeout."),
            "Request timeout. No alternate Qwen account available after failure.");
  EXPECT_EQ(QwenProvider::composeNoAlternateAccountError(""),
            "No alternate Qwen account available after failure.");
}

TEST(QwenProvider, ComposeNoAlternateAccountErrorPreservesRawProviderBody) {
  const std::string cause = QwenProvider::formatErrorMessage(
      "qwen3-coder-plus", 429, R"({"error":{"message":"quota exceeded"}})",
      "Quota exhausted or rate limited. Switching to next account...");

  const std::string message =
      QwenProvider::composeNoAlternateAccountError(cause);

  EXPECT_NE(message.find("Raw provider body:"), std::string::npos);
  EXPECT_NE(message.find(R"({"error":{"message":"quota exceeded"}})"),
            std::string::npos);
  EXPECT_NE(message.find("No alternate Qwen account available after failure."),
            std::string::npos);
}

TEST(QwenProvider, FormatErrorMessageIncludesProviderModelAndRawBody) {
  const std::string body = R"({"error":{"message":"bad request"}})";

  const std::string message =
      QwenProvider::formatErrorMessage("qwen3-coder-plus", 400, body,
                                       "Request validation failed.");

  EXPECT_NE(message.find("Request validation failed. (HTTP 400)"),
            std::string::npos);
  EXPECT_NE(message.find("Provider: qwen"), std::string::npos);
  EXPECT_NE(message.find("Model: qwen3-coder-plus"), std::string::npos);
  EXPECT_NE(message.find("Raw provider body:\n" + body), std::string::npos);
}

TEST(QwenProvider, BuildRequestPayloadDropsMalformedHistoricalToolCalls) {
  AgentHistory history;
  history.turns.push_back(makeTurn(
      "assistant-1",
      makeMessage(Role::Assistant,
                  {ToolCallContent{"call-bad", "file_editfile_edit",
                                   R"({"path":"a"})"},
                   TextContent{"I tried something."}})));
  history.turns.push_back(makeTurn(
      "tool-1",
      makeMessage(Role::ToolResult,
                  {ToolResultContent{"call-bad", "{}", true, "", ""}})));

  ProviderOptions opts;
  opts.tools.push_back(ToolDefinition{"file_edit", "edit files",
                                      R"({"type":"object"})"});

  auto payload = QwenProvider::buildRequestPayload(history, opts);
  auto doc = parsePayload(payload.body);

  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc["messages"].IsArray());
  ASSERT_EQ(payload.droppedToolCalls, 1u);
  ASSERT_EQ(payload.droppedToolResults, 1u);
  ASSERT_FALSE(payload.warnings.empty());

  const auto &messages = doc["messages"];
  ASSERT_EQ(messages.Size(), 2u);
  EXPECT_FALSE(messages[0].HasMember("tool_calls"));
  ASSERT_TRUE(messages[1]["content"].IsArray());
  EXPECT_EQ(std::string(messages[1]["role"].GetString()), "system");
}

TEST(QwenProvider, BuildRequestPayloadNormalizesValidToolCalls) {
  AgentHistory history;
  history.turns.push_back(makeTurn(
      "assistant-1",
      makeMessage(Role::Assistant,
                  {ToolCallContent{"call-1", "file_edit",
                                   "{\n  \"path\": \"a\"\n}"},
                   TextContent{"Applying edit."}})));
  history.turns.push_back(makeTurn(
      "tool-1",
      makeMessage(Role::ToolResult,
                  {ToolResultContent{"call-1", "{\"ok\":true}", true, "", ""}})));

  ProviderOptions opts;
  opts.tools.push_back(ToolDefinition{"file_edit", "edit files",
                                      R"({"type":"object"})"});

  auto payload = QwenProvider::buildRequestPayload(history, opts);
  auto doc = parsePayload(payload.body);

  ASSERT_FALSE(doc.HasParseError());
  ASSERT_EQ(payload.droppedToolCalls, 0u);
  ASSERT_EQ(payload.droppedToolResults, 0u);

  const auto &messages = doc["messages"];
  ASSERT_EQ(messages.Size(), 2u);
  ASSERT_TRUE(messages[0]["tool_calls"].IsArray());
  ASSERT_EQ(messages[0]["tool_calls"].Size(), 1u);
  EXPECT_EQ(std::string(messages[0]["tool_calls"][0]["function"]["name"].GetString()),
            "file_edit");
  EXPECT_EQ(std::string(messages[0]["tool_calls"][0]["function"]["arguments"].GetString()),
            "{\"path\":\"a\"}");
  EXPECT_EQ(std::string(messages[1]["tool_call_id"].GetString()), "call-1");
}

TEST(QwenProvider, BuildRequestPayloadDropsInvalidJsonArguments) {
  AgentHistory history;
  history.turns.push_back(makeTurn(
      "assistant-1",
      makeMessage(Role::Assistant,
                  {ToolCallContent{"call-1", "file_edit",
                                   R"({"path":"a"}{"path":"b"})"}})));

  ProviderOptions opts;
  opts.tools.push_back(ToolDefinition{"file_edit", "edit files",
                                      R"({"type":"object"})"});

  auto payload = QwenProvider::buildRequestPayload(history, opts);
  auto doc = parsePayload(payload.body);

  ASSERT_FALSE(doc.HasParseError());
  ASSERT_EQ(payload.droppedToolCalls, 1u);
  ASSERT_TRUE(doc["messages"].IsArray());
  ASSERT_EQ(doc["messages"].Size(), 1u);
  EXPECT_EQ(std::string(doc["messages"][0]["role"].GetString()), "system");
}

TEST(QwenProvider, MeaningfulStreamEventDetectsTextThinkingAndToolProgress) {
  EXPECT_TRUE(QwenProvider::isMeaningfulStreamEvent(TextChunk{"hello"}));
  EXPECT_TRUE(QwenProvider::isMeaningfulStreamEvent(
      ThinkingChunk{"reasoning", ""}));
  EXPECT_TRUE(QwenProvider::isMeaningfulStreamEvent(
      ToolCallChunk{"tool-1", 0, "file_edit", ""}));
  EXPECT_TRUE(QwenProvider::isMeaningfulStreamEvent(
      ToolCallChunk{"tool-1", 0, "", "{\"path\":\"a\"}"}));

  EXPECT_FALSE(
      QwenProvider::isMeaningfulStreamEvent(TextChunk{""}));
  EXPECT_FALSE(
      QwenProvider::isMeaningfulStreamEvent(ThinkingChunk{"", ""}));
  EXPECT_FALSE(QwenProvider::isMeaningfulStreamEvent(
      ToolCallChunk{"tool-1", 0, "", ""}));
  EXPECT_FALSE(QwenProvider::isMeaningfulStreamEvent(
      StreamRetrying{1, 5, 500, 1000, "retry", "acct"}));
  EXPECT_FALSE(QwenProvider::isMeaningfulStreamEvent(
      StreamError{"boom", 500, "acct"}));
}

TEST(QwenProvider, ValidateCompletedToolCallBatchFlagsIncompleteArgs) {
  const std::vector<ToolCallChunk> calls = {
      ToolCallChunk{"call-1", 0, "chunk_add",
                    R"({"plan_id":"plan-1","title":"Incomplete")"}};

  auto error = QwenProvider::validateCompletedToolCallBatch(calls);
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->find("incomplete tool-call arguments for tool 'chunk_add'"),
            std::string::npos);
  EXPECT_NE(
      error->find("Provider stream truncated during tool-call generation."),
      std::string::npos);
}

TEST(QwenProvider, ValidateCompletedToolCallBatchFlagsMissingName) {
  const std::vector<ToolCallChunk> calls = {
      ToolCallChunk{"call-1", 0, "", R"({"plan_id":"plan-1"})"}};

  auto error = QwenProvider::validateCompletedToolCallBatch(calls);
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->find("missing tool name"), std::string::npos);
}

TEST(QwenProvider, ValidateCompletedToolCallBatchAcceptsValidMultiToolBatch) {
  const std::vector<ToolCallChunk> calls = {
      ToolCallChunk{"call-1", 0, "chunk_add",
                    R"({"plan_id":"plan-1","title":"A"})"},
      ToolCallChunk{"call-2", 1, "chunk_add",
                    R"({"plan_id":"plan-1","title":"B"})"}};

  auto error = QwenProvider::validateCompletedToolCallBatch(calls);
  EXPECT_FALSE(error.has_value());
}
