#include "Context.hpp"
#include "Enums.hpp"
#include "Events.hpp"
#include "Message.hpp"
#include "Metrics.hpp"
#include "Serialization.hpp"
#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>
#include <vector>

using namespace firmius::shared;

namespace {

AgentContext createTestContext() {
  AgentContext context;

  context.identity.id = "agent-001";
  context.identity.name = "TestAgent";
  context.identity.role = "assistant";
  context.identity.goal = "Help with testing";
  context.identity.systemPrompt = "You are a helpful assistant.";

  context.permissions.allowedScopes = {ToolScope::FilesystemRead,
                                       ToolScope::Process, ToolScope::Git};
  context.permissions.allowedPaths = {"/home/user", "/tmp"};
  context.permissions.allowOutsideCwd = false;

  context.environment.type = HostType::Local;
  context.environment.identifier = "local-machine";
  context.environment.cwd = "/home/nunya/Projects/Firmius";
  context.environment.envVars = {
      {"PATH", "/usr/bin:/bin"}, {"HOME", "/home/nunya"}, {"USER", "nunya"}};

  context.history->threadId = "thread-123";

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
  msg2.content.push_back(ThinkingContent{"Let me think about how to respond.", ""});
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

  context.history->turns.push_back(turn1);
  context.history->turns.push_back(turn2);

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

} // namespace

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
  msg.content.push_back(ThinkingContent{"Let me think", ""});
  msg.content.push_back(
      ToolCallContent{"call-1", "test_tool", R"({"key":"value"})"});
  msg.content.push_back(
      ToolResultContent{"call-1", R"({"result":"ok"})", true, "", ""});
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

TEST(Serialization, ThreadMetadataPermissionModeRoundtrip) {
  ThreadMetadata metadata;
  metadata.threadId = "thread-123";
  metadata.title = "Permissions";
  metadata.hostOptions.type = HostType::Local;
  metadata.hostIdentifier = "localhost";
  metadata.cwd = "/tmp";
  metadata.leadPersona = "firmius";
  metadata.activePlanId = "plan-123";
  metadata.permissionMode = ThreadPermissionMode::DenyAll;
  metadata.createdAt = 123;
  metadata.lastActiveAt = 456;

  auto doc = toJson(metadata);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  rapidjson::Document parsed;
  parsed.Parse(buffer.GetString());
  auto roundtripped = threadMetadataFromJson(parsed);

  EXPECT_EQ(metadata, roundtripped);
}

TEST(Serialization, ThreadMetadataPermissionModeDefaultsToRequest) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &a = doc.GetAllocator();
  doc.AddMember("threadId", "thread-legacy", a);
  doc.AddMember("title", "Legacy", a);
  doc.AddMember("cwd", "/tmp", a);
  doc.AddMember("leadPersona", "firmius", a);

  auto metadata = threadMetadataFromJson(doc);
  EXPECT_EQ(metadata.permissionMode, ThreadPermissionMode::Request);
  EXPECT_TRUE(metadata.activePlanId.empty());
}

TEST(Serialization, ThreadMetadataActivePlanIdBackwardCompatibleDefault) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &a = doc.GetAllocator();
  doc.AddMember("threadId", "thread-legacy", a);
  doc.AddMember("title", "Legacy", a);
  doc.AddMember("cwd", "/tmp", a);
  doc.AddMember("leadPersona", "firmius", a);
  doc.AddMember("permissionMode", "Request", a);

  auto metadata = threadMetadataFromJson(doc);
  EXPECT_EQ(metadata.threadId, "thread-legacy");
  EXPECT_TRUE(metadata.activePlanId.empty());
}

TEST(Serialization, WorkChunkRoundtrip) {
  WorkChunk original;
  original.id = "chunk-1";
  original.title = "Add models";
  original.goal = "Create persisted work chunk model";
  original.context = "Chunk 1";
  original.constraints = "No chunk files";
  original.completion = "Model compiles and round-trips";
  original.status = WorkChunkStatus::Verifying;
  original.priority = 7;
  original.dependsOn = {"chunk-0", "chunk-bootstrap"};
  original.assignedAgentId = "agent-lead";
  original.assignedRole = "implementer";
  original.attemptCount = 2;
  original.resultSummary = "Implemented model";
  original.reviewSummary = "Pending review";
  original.createdAt = 100;
  original.updatedAt = 200;

  auto doc = toJson(original);
  auto restored = workChunkFromJson(doc);

  EXPECT_EQ(original, restored);
}

TEST(Serialization, WorkChunkDefaultsForMissingFields) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &a = doc.GetAllocator();
  doc.AddMember("id", "chunk-legacy", a);
  doc.AddMember("title", "Legacy chunk", a);

  auto chunk = workChunkFromJson(doc);
  EXPECT_EQ(chunk.id, "chunk-legacy");
  EXPECT_EQ(chunk.title, "Legacy chunk");
  EXPECT_EQ(chunk.status, WorkChunkStatus::Draft);
  EXPECT_EQ(chunk.priority, 0);
  EXPECT_EQ(chunk.attemptCount, 0);
  EXPECT_TRUE(chunk.dependsOn.empty());
  EXPECT_TRUE(chunk.assignedAgentId.empty());
}

TEST(Serialization, PlanRoundtrip) {
  Plan original;
  original.id = "plan-1";
  original.threadId = "thread-1";
  original.title = "Work Language Migration";
  original.objective = "Add plan persistence";
  original.context = "Chunk 1 only";
  original.strategy = "Models plus persistence";
  original.status = PlanStatus::Active;
  original.notes = "No tool APIs yet";
  original.createdAt = 123;
  original.updatedAt = 456;

  WorkChunk chunk;
  chunk.id = "chunk-1";
  chunk.title = "Add core models";
  chunk.goal = "Define Plan and WorkChunk";
  chunk.status = WorkChunkStatus::Ready;
  chunk.priority = 1;
  chunk.createdAt = 124;
  chunk.updatedAt = 125;
  original.chunks.push_back(chunk);

  auto doc = toJson(original);
  auto restored = planFromJson(doc);

  EXPECT_EQ(original, restored);
}

TEST(Serialization, PlanDefaultsForMissingFields) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &a = doc.GetAllocator();
  doc.AddMember("id", "plan-legacy", a);
  doc.AddMember("thread_id", "thread-legacy", a);
  doc.AddMember("title", "Legacy plan", a);

  auto plan = planFromJson(doc);
  EXPECT_EQ(plan.id, "plan-legacy");
  EXPECT_EQ(plan.threadId, "thread-legacy");
  EXPECT_EQ(plan.title, "Legacy plan");
  EXPECT_EQ(plan.status, PlanStatus::Draft);
  EXPECT_EQ(plan.createdAt, 0u);
  EXPECT_TRUE(plan.chunks.empty());
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

  StreamEvent toolEvent =
      ToolCallChunk{"call-1", 0, "readFile", R"({"path":"test"})"};
  doc = toJson(toolEvent);
  buffer.Clear();
  writer.Reset(buffer);
  doc.Accept(writer);
  json = buffer.GetString();

  parsed.Parse(json.c_str());
  auto roundtrippedTool = streamEventFromJsonValue(parsed);

  EXPECT_EQ(std::get<ToolCallChunk>(toolEvent),
            std::get<ToolCallChunk>(roundtrippedTool));
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

TEST(Serialization, TokenMetricsCacheFields) {
  AgentMetrics original;
  original.tokens.prompt = 500;
  original.tokens.completion = 200;
  original.tokens.reasoning = 50;
  original.tokens.cacheRead = 150;
  original.tokens.cacheWrite = 75;
  original.tokens.total = 975;
  original.timing.startMs = 1000;
  original.timing.firstTokenMs = 1050;
  original.timing.endMs = 2000;
  original.timing.toolExecutionMs = 300;
  original.estimatedCostUsd = 0.0042;

  auto doc = toJson(original);
  auto restored = agentMetricsFromJsonValue(doc);

  EXPECT_EQ(original, restored);
  EXPECT_EQ(restored.tokens.cacheRead, 150u);
  EXPECT_EQ(restored.tokens.cacheWrite, 75u);
}

TEST(Serialization, TokenMetricsBackwardCompat) {
  // Simulate old-format JSON without cacheRead/cacheWrite
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();

  rapidjson::Value tokens(rapidjson::kObjectType);
  tokens.AddMember("prompt", 100u, a);
  tokens.AddMember("completion", 50u, a);
  tokens.AddMember("reasoning", 10u, a);
  tokens.AddMember("total", 160u, a);
  d.AddMember("tokens", tokens, a);

  rapidjson::Value timing(rapidjson::kObjectType);
  timing.AddMember("startMs", uint64_t(0), a);
  timing.AddMember("firstTokenMs", uint64_t(0), a);
  timing.AddMember("endMs", uint64_t(0), a);
  timing.AddMember("toolExecutionMs", uint64_t(0), a);
  d.AddMember("timing", timing, a);

  d.AddMember("estimatedCostUsd", 0.0, a);

  auto restored = agentMetricsFromJsonValue(d);

  EXPECT_EQ(restored.tokens.prompt, 100u);
  EXPECT_EQ(restored.tokens.cacheRead, 0u);
  EXPECT_EQ(restored.tokens.cacheWrite, 0u);
  EXPECT_EQ(restored.tokens.total, 160u);
}

TEST(Serialization, ImageContentRoundtrip) {
  MessagePart original =
      ImageContent{"https://example.com/img.png", "image/png", "high"};

  auto doc = toJson(original);
  auto restored = messagePartFromJsonValue(doc);

  EXPECT_EQ(original, restored);
  auto *img = std::get_if<ImageContent>(&restored);
  ASSERT_NE(img, nullptr);
  EXPECT_EQ(img->url, "https://example.com/img.png");
  EXPECT_EQ(img->mediaType, "image/png");
  EXPECT_EQ(img->detail, "high");
}

TEST(Serialization, StreamDoneRoundtrip) {
  StreamEvent original = StreamDone{StopReason::ToolUse};

  auto doc = toJson(original);
  auto restored = streamEventFromJsonValue(doc);

  EXPECT_EQ(original, restored);
  auto *done = std::get_if<StreamDone>(&restored);
  ASSERT_NE(done, nullptr);
  EXPECT_EQ(done->reason, StopReason::ToolUse);
}

TEST(Serialization, StreamErrorRoundtrip) {
  StreamEvent original = StreamError{"Rate limit exceeded", 429, ""};

  auto doc = toJson(original);
  auto restored = streamEventFromJsonValue(doc);

  EXPECT_EQ(original, restored);
  auto *err = std::get_if<StreamError>(&restored);
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(err->message, "Rate limit exceeded");
  EXPECT_EQ(err->httpStatus, 429);
}

TEST(Serialization, StopReasonOnAgentTurn) {
  AgentTurn original;
  original.turnId = "turn-sr-1";
  original.stopReason = StopReason::MaxTokens;
  Message msg;
  msg.id = "msg-sr-1";
  msg.role = Role::Assistant;
  msg.content.push_back(TextContent{"truncated output"});
  msg.timestamp = 1700000000000;
  original.messages.push_back(msg);

  auto doc = toJson(original);
  auto restored = agentTurnFromJsonValue(doc);

  EXPECT_EQ(restored.stopReason, StopReason::MaxTokens);
  EXPECT_EQ(restored.turnId, "turn-sr-1");
}

TEST(Serialization, AgentConfigRoundtrip) {
  AgentConfig original;
  original.modelId = "gpt-4o";
  original.personaName = "architect";
  original.maxTurns = 50;
  original.temperature = 0.3f;
  original.maxTokens = 4096;
  original.stop = {"<stop />", "###"};
  original.persistHistory = false;

  auto doc = toJson(original);
  auto restored = agentConfigFromJsonValue(doc);

  EXPECT_EQ(original, restored);
  EXPECT_EQ(restored.modelId, "gpt-4o");
  EXPECT_EQ(restored.maxTurns, 50);
  EXPECT_EQ(restored.persistHistory, false);
  ASSERT_EQ(restored.stop.size(), 2u);
  EXPECT_EQ(restored.stop[0], "<stop />");
}

TEST(Serialization, AgentConfigInContext) {
  AgentContext original;
  original.identity = {
      "id1", "TestAgent", "coder", "solve bugs", "You are a coder.", "", ""};
  original.permissions.allowedScopes = {ToolScope::FilesystemRead,
                                        ToolScope::Process};
  original.permissions.allowedPaths = {"/work"};
  original.environment.type = HostType::Local;
  original.environment.identifier = "local";
  original.environment.cwd = "/work";
  original.history->threadId = "thread-cfg-1";
  original.config.modelId = "claude-opus-4";
  original.config.personaName = "architect";
  original.config.maxTurns = 100;
  original.config.temperature = 0.1f;
  original.config.maxTokens = 8192;
  original.config.stop = {"STOP"};
  original.config.persistHistory = false;

  std::string json = serializeToString(original);
  auto restored = deserializeFromString(json);

  EXPECT_EQ(restored.config.modelId, "claude-opus-4");
  EXPECT_EQ(restored.config.personaName, "architect");
  EXPECT_EQ(restored.config.maxTurns, 100);
  EXPECT_FLOAT_EQ(restored.config.temperature, 0.1f);
  ASSERT_TRUE(restored.config.maxTokens.has_value());
  EXPECT_EQ(restored.config.maxTokens.value(), 8192u);
  EXPECT_EQ(restored.config.persistHistory, false);
}

TEST(Serialization, ModelInfoRoundtrip) {
  ModelInfo original;
  original.id = "gpt-4o-2024-08-06";
  original.provider = "openai";
  original.contextWindow = 128000;
  original.modalities = {"text", "image"};
  original.supportsReasoning = true;
  original.pricePer1MInput = 2.50;
  original.pricePer1MOutput = 10.0;
  original.pricePer1MCacheRead = 1.25;
  original.pricePer1MCacheWrite = 3.75;

  auto doc = toJson(original);
  auto restored = modelInfoFromJsonValue(doc);

  EXPECT_EQ(original, restored);
  EXPECT_EQ(restored.id, "gpt-4o-2024-08-06");
  EXPECT_DOUBLE_EQ(restored.pricePer1MCacheRead, 1.25);
  EXPECT_DOUBLE_EQ(restored.pricePer1MCacheWrite, 3.75);
  ASSERT_EQ(restored.modalities.size(), 2u);
}

TEST(Serialization, AgentMetricsAccumulation) {
  AgentMetrics turn1;
  turn1.tokens = {
      100, 50,  10, 20, 5,
      185, 100, 150}; // prompt, completion, reasoning, cacheRead, cacheWrite,
                      // contextSize, cumulativePrompt, total
  turn1.timing = {1000, 1050, 2000, 300};
  turn1.estimatedCostUsd = 0.005;

  AgentMetrics turn2;
  turn2.tokens = {
      200, 100, 20, 40, 10,
      370, 200, 300}; // prompt, completion, reasoning, cacheRead, cacheWrite,
                      // contextSize, cumulativePrompt, total
  turn2.timing = {2500, 2520, 3500, 150};
  turn2.estimatedCostUsd = 0.010;

  AgentMetrics aggregate;
  aggregate += turn1;
  aggregate += turn2;

  // Tokens: additive
  EXPECT_EQ(aggregate.tokens.prompt, 300u);
  EXPECT_EQ(aggregate.tokens.completion, 150u);
  EXPECT_EQ(aggregate.tokens.cacheRead, 60u);
  EXPECT_EQ(aggregate.tokens.cacheWrite, 15u);
  EXPECT_EQ(aggregate.tokens.total, 450u); // 150 + 300 from accumulated metrics

  // Timing: min for start/firstToken, max for end, additive for toolExecution
  EXPECT_EQ(aggregate.timing.startMs, 1000u);
  EXPECT_EQ(aggregate.timing.firstTokenMs, 1050u);
  EXPECT_EQ(aggregate.timing.endMs, 3500u);
  EXPECT_EQ(aggregate.timing.toolExecutionMs, 450u);

  // Cost: additive
  EXPECT_DOUBLE_EQ(aggregate.estimatedCostUsd, 0.015);
}

TEST(Serialization, CancelledStatusRoundtrip) {
  AgentContext original;
  original.identity = {"id1", "Test", "coder", "test", "prompt", "", ""};
  original.permissions.allowedScopes = {ToolScope::Process};
  original.permissions.allowedPaths = {"/work"};
  original.environment.type = HostType::Local;
  original.environment.identifier = "local";
  original.environment.cwd = "/work";
  original.history->threadId = "thread-cancel-1";
  original.state.currentStatus = AgentStatus::Cancelled;
  original.state.fatalError = "User interrupted";
  original.state.editedFiles = {"/work/test.py", "/work/README.md"};
  original.state.completedActions = {"Created test file",
                                     "Updated documentation"};

  std::string json = serializeToString(original);
  auto restored = deserializeFromString(json);

  EXPECT_EQ(restored.state.currentStatus, AgentStatus::Cancelled);
  ASSERT_TRUE(restored.state.fatalError.has_value());
  EXPECT_EQ(restored.state.fatalError.value(), "User interrupted");
  EXPECT_EQ(restored.state.editedFiles.size(), 2u);
  EXPECT_EQ(restored.state.editedFiles[0], "/work/test.py");
  EXPECT_EQ(restored.state.editedFiles[1], "/work/README.md");
  EXPECT_EQ(restored.state.completedActions.size(), 2u);
  EXPECT_EQ(restored.state.completedActions[0], "Created test file");
  EXPECT_EQ(restored.state.completedActions[1], "Updated documentation");
}
