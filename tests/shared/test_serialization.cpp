#include <gtest/gtest.h>
#include "Serialization.hpp"
#include "Context.hpp"
#include "Message.hpp"
#include "Metrics.hpp"
#include "Events.hpp"
#include "Enums.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>
#include <vector>
#include <map>
#include <optional>

using namespace firmius::shared;

namespace {

AgentContext createTestContext() {
  AgentContext context;

  context.identity.id = "agent-001";
  context.identity.name = "TestAgent";
  context.identity.role = "assistant";
  context.identity.goal = "Help with testing";
  context.identity.systemPrompt = "You are a helpful assistant.";

  context.permissions.allowedScopes = {ToolScope::FilesystemRead, ToolScope::Process, ToolScope::Git};
  context.permissions.allowedPaths = {"/home/user", "/tmp"};
  context.permissions.allowOutsideCwd = false;

  context.environment.type = HostType::Local;
  context.environment.identifier = "local-machine";
  context.environment.cwd = "/home/nunya/Projects/Firmius";
  context.environment.envVars = {
    {"PATH", "/usr/bin:/bin"},
    {"HOME", "/home/nunya"},
    {"USER", "nunya"}
  };

  context.history.threadId = "thread-123";

  AgentTurn turn1;
  turn1.turnId = "turn-001";
  turn1.metrics.tokens.prompt = 100;
  turn1.metrics.tokens.completion = 50;
  turn1.metrics.tokens.reasoning = 20;
  turn1.metrics.tokens.total = 170;
  turn1.metrics.timing.startMs = 1000;
  turn1.metrics.timing.firstTokenMs = 1100;
  turn1.metrics.timing.endMs = 2000;
  turn1.metrics.timing.toolExecutionMs = 500;
  turn1.metrics.estimatedCostUsd = 0.002;

  Message msg1;
  msg1.id = "msg-001";
  msg1.role = Role::User;
  msg1.content.push_back(TextContent{"Hello, can you help me?"});
  msg1.timestamp = 1000;
  msg1.parentId = std::nullopt;

  Message msg2;
  msg2.id = "msg-002";
  msg2.role = Role::Assistant;
  msg2.content.push_back(TextContent{"Sure, I'd be happy to help!"});
  msg2.content.push_back(ThinkingContent{"Let me think about how to respond."});
  msg2.timestamp = 1500;
  msg2.parentId = "msg-001";

  Message msg3;
  msg3.id = "msg-003";
  msg3.role = Role::Assistant;
  ToolCallContent toolCall;
  toolCall.id = "call-001";
  toolCall.name = "read_file";
  toolCall.args = R"({"path": "/home/user/test.txt"})";
  msg3.content.push_back(toolCall);
  msg3.timestamp = 2000;
  msg3.parentId = "msg-002";

  Message msg4;
  msg4.id = "msg-004";
  msg4.role = Role::ToolResult;
  ToolResultContent toolResult;
  toolResult.toolCallId = "call-001";
  toolResult.result = R"({"content": "file contents here"})";
  toolResult.success = true;
  msg4.content.push_back(toolResult);
  msg4.timestamp = 2500;
  msg4.parentId = "msg-003";

  turn1.messages.push_back(msg1);
  turn1.messages.push_back(msg2);
  turn1.messages.push_back(msg3);
  turn1.messages.push_back(msg4);

  AgentTurn turn2;
  turn2.turnId = "turn-002";
  turn2.metrics.tokens.prompt = 150;
  turn2.metrics.tokens.completion = 75;
  turn2.metrics.tokens.reasoning = 30;
  turn2.metrics.tokens.total = 255;
  turn2.metrics.timing.startMs = 3000;
  turn2.metrics.timing.firstTokenMs = 3100;
  turn2.metrics.timing.endMs = 4000;
  turn2.metrics.timing.toolExecutionMs = 800;
  turn2.metrics.estimatedCostUsd = 0.003;

  Message msg5;
  msg5.id = "msg-005";
  msg5.role = Role::User;
  msg5.content.push_back(TextContent{"Can you also check the git status?"});
  msg5.timestamp = 3000;
  msg5.parentId = std::nullopt;

  Message msg6;
  msg6.id = "msg-006";
  msg6.role = Role::Assistant;
  msg6.content.push_back(TextContent{"Let me check that for you."});
  msg6.timestamp = 3500;
  msg6.parentId = "msg-005";

  turn2.messages.push_back(msg5);
  turn2.messages.push_back(msg6);

  context.history.turns.push_back(turn1);
  context.history.turns.push_back(turn2);

  context.state.currentStatus = AgentStatus::Idle;
  context.state.pendingToolCalls = {"call-002", "call-003"};
  context.state.ownedProcesses = {"proc-123", "proc-456"};
  context.state.fatalError = std::nullopt;

  context.aggregateMetrics.tokens.prompt = 250;
  context.aggregateMetrics.tokens.completion = 125;
  context.aggregateMetrics.tokens.reasoning = 50;
  context.aggregateMetrics.tokens.total = 425;
  context.aggregateMetrics.timing.startMs = 1000;
  context.aggregateMetrics.timing.firstTokenMs = 1100;
  context.aggregateMetrics.timing.endMs = 4000;
  context.aggregateMetrics.timing.toolExecutionMs = 1300;
  context.aggregateMetrics.estimatedCostUsd = 0.005;

  return context;
}

}

TEST(Serialization, AgentContextRoundtrip) {
  auto original = createTestContext();
  std::string json = serializeToString(original);
  auto deserialized = deserializeFromString(json);
  EXPECT_EQ(original, deserialized);
}

TEST(Serialization, MessageRoundtrip) {
  Message msg;
  msg.id = "msg-test";
  msg.role = Role::Assistant;
  msg.content.push_back(TextContent{"Hello world"});
  msg.content.push_back(ThinkingContent{"Let me think"});
  msg.content.push_back(ToolCallContent{"call-1", "test_tool", R"({"key":"value"})"});
  msg.content.push_back(ToolResultContent{"call-1", R"({"result":"ok"})", true});
  msg.timestamp = 12345;
  msg.parentId = "parent-msg";

  auto doc = toJson(msg);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  std::string json = buffer.GetString();

  rapidjson::Document parsed;
  parsed.Parse(json.c_str());
  auto roundtripped = messageFromJsonValue(parsed);

  EXPECT_EQ(msg, roundtripped);
}

TEST(Serialization, AgentMetricsRoundtrip) {
  AgentMetrics metrics;
  metrics.tokens.prompt = 100;
  metrics.tokens.completion = 50;
  metrics.tokens.reasoning = 25;
  metrics.tokens.total = 175;
  metrics.timing.startMs = 1000;
  metrics.timing.firstTokenMs = 1100;
  metrics.timing.endMs = 2000;
  metrics.timing.toolExecutionMs = 500;
  metrics.estimatedCostUsd = 0.0015;

  auto doc = toJson(metrics);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  std::string json = buffer.GetString();

  rapidjson::Document parsed;
  parsed.Parse(json.c_str());
  auto roundtripped = agentMetricsFromJsonValue(parsed);

  EXPECT_EQ(metrics, roundtripped);
}

TEST(Serialization, StreamEventRoundtrip) {
  StreamEvent textEvent = TextChunk{"Hello"};
  auto doc = toJson(textEvent);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  std::string json = buffer.GetString();

  rapidjson::Document parsed;
  parsed.Parse(json.c_str());
  auto roundtripped = streamEventFromJsonValue(parsed);

  EXPECT_EQ(std::get<TextChunk>(textEvent), std::get<TextChunk>(roundtripped));

  StreamEvent toolEvent = ToolCallChunk{"call-1", 0, "readFile", R"({"path":"test"})"};
  doc = toJson(toolEvent);
  buffer.Clear();
  writer.Reset(buffer);
  doc.Accept(writer);
  json = buffer.GetString();

  parsed.Parse(json.c_str());
  auto roundtrippedTool = streamEventFromJsonValue(parsed);

  EXPECT_EQ(std::get<ToolCallChunk>(toolEvent), std::get<ToolCallChunk>(roundtrippedTool));
}

TEST(Serialization, OptionalFields) {
  Message msgWithParent;
  msgWithParent.id = "msg-1";
  msgWithParent.role = Role::User;
  msgWithParent.content.push_back(TextContent{"Test"});
  msgWithParent.timestamp = 1000;
  msgWithParent.parentId = "parent-123";

  Message msgWithoutParent;
  msgWithoutParent.id = "msg-2";
  msgWithoutParent.role = Role::Assistant;
  msgWithoutParent.content.push_back(TextContent{"Response"});
  msgWithoutParent.timestamp = 2000;
  msgWithoutParent.parentId = std::nullopt;

  auto doc1 = toJson(msgWithParent);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc1.Accept(writer);
  rapidjson::Document parsed1;
  parsed1.Parse(buffer.GetString());
  auto roundtrip1 = messageFromJsonValue(parsed1);

  auto doc2 = toJson(msgWithoutParent);
  buffer.Clear();
  writer.Reset(buffer);
  doc2.Accept(writer);
  rapidjson::Document parsed2;
  parsed2.Parse(buffer.GetString());
  auto roundtrip2 = messageFromJsonValue(parsed2);

  EXPECT_EQ(msgWithParent, roundtrip1);
  EXPECT_EQ(msgWithoutParent, roundtrip2);
  EXPECT_FALSE(roundtrip2.parentId.has_value());
}
