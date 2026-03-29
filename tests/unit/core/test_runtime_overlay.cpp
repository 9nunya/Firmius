#include "agents/RuntimeOverlay.hpp"
#include "environment/Workspace.hpp"
#include "persistence/ThreadManager.hpp"
#include "../mocks/MockHost.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

std::string firstTextContent(const AgentTurn& turn) {
  for (const auto& message : turn.messages) {
    for (const auto& part : message.content) {
      if (const auto* text = std::get_if<TextContent>(&part)) {
        return text->text;
      }
    }
  }
  return "";
}

const ToolResultContent* findToolResultByCallId(const AgentHistory& history,
                                                const std::string& callId) {
  for (const auto& turn : history.turns) {
    for (const auto& message : turn.messages) {
      for (const auto& part : message.content) {
        if (const auto* result = std::get_if<ToolResultContent>(&part)) {
          if (result->toolCallId == callId) {
            return result;
          }
        }
      }
    }
  }
  return nullptr;
}

std::string fileReadContentField(const ToolResultContent& result) {
  rapidjson::Document doc;
  doc.Parse(result.result.c_str());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("content") ||
      !doc["content"].IsString()) {
    return "";
  }
  return doc["content"].GetString();
}

std::string fileEditUpdatedFilesField(const ToolResultContent& result) {
  rapidjson::Document doc;
  doc.Parse(result.result.c_str());
  if (doc.HasParseError() || !doc.IsObject() ||
      !doc.HasMember("updated_files") || !doc["updated_files"].IsString()) {
    return "";
  }
  return doc["updated_files"].GetString();
}

std::size_t countOccurrences(const std::string& haystack,
                             const std::string& needle) {
  if (needle.empty()) {
    return 0;
  }

  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

class RuntimeOverlayTest : public ::testing::Test {
protected:
  void SetUp() override {
    char tempTemplate[] = "/tmp/firmius_runtime_overlay_XXXXXX";
    char* result = mkdtemp(tempTemplate);
    ASSERT_NE(result, nullptr);
    tempDir_ = result;

    const char* existingHome = std::getenv("HOME");
    if (existingHome) {
      originalHome_ = existingHome;
    }
    setenv("HOME", tempDir_.c_str(), 1);
    std::filesystem::create_directories(tempDir_ + "/.firmius/threads");

    threadManager_ =
        std::make_unique<ThreadManager>(tempDir_ + "/.firmius/threads");
    ThreadMetadata metadata;
    metadata.title = "Runtime Overlay Test";
    metadata.hostOptions.type = HostType::Local;
    metadata.cwd = "/work";
    metadata.leadPersona = "lead";
    threadId_ = threadManager_->createThread(metadata);

    context_.history = std::make_shared<AgentHistory>();
    context_.history->threadId = threadId_;
    context_.identity.id = "lead-agent";
    context_.config.personaName = "lead";
    workspace_ = std::make_unique<Workspace>("/work");
    host_ = std::make_shared<firmius::test::MockHost>();
  }

  void TearDown() override {
    if (!originalHome_.empty()) {
      setenv("HOME", originalHome_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
    std::filesystem::remove_all(tempDir_);
  }

  void setFile(const std::string& path, const std::string& content) {
    host_->writeFile(path, std::vector<uint8_t>(content.begin(), content.end()));
  }

  std::string tempDir_;
  std::string originalHome_;
  std::string threadId_;
  std::unique_ptr<ThreadManager> threadManager_;
  AgentContext context_;
  std::unique_ptr<Workspace> workspace_;
  std::shared_ptr<firmius::test::MockHost> host_;
};

TEST_F(RuntimeOverlayTest, BuildRequestHistoryAppendsLeadStateOnly) {
  Plan plan;
  plan.threadId = threadId_;
  plan.title = "Ship Runtime Overlay";
  plan.objective = "Keep live runtime state out of journal";
  plan.strategy = "Use ephemeral provider overlays";
  WorkChunk chunk;
  chunk.id = "chunk-1";
  chunk.title = "Implement overlay builder";
  chunk.goal = "Append live turns only for provider requests";
  WorkTask task;
  task.id = "task-1";
  task.title = "Render watched files";
  task.goal = "Show current file contents";
  chunk.tasks.push_back(task);
  plan.chunks.push_back(chunk);
  const std::string planId = threadManager_->createPlan(plan);

  auto metadata = threadManager_->getMetadata(threadId_);
  metadata.activePlanId = planId;
  threadManager_->updateMetadata(threadId_, metadata);

  AgentTodoList todo;
  todo.threadId = threadId_;
  todo.agentId = context_.identity.id;
  todo.nextId = 2;
  TodoItem item;
  item.id = 1;
  item.text = "Review live overlay content";
  item.status = TodoStatus::Pending;
  item.createdAt = 1;
  item.updatedAt = 1;
  todo.items.push_back(item);
  threadManager_->writeAgentTodo(threadId_, context_.identity.id, todo);

  AgentLiveState liveState;
  liveState.threadId = threadId_;
  liveState.agentId = context_.identity.id;
  WatchedFileState watchedFile;
  watchedFile.path = "/work/sample.ts";
  watchedFile.fullyRead = true;
  watchedFile.terminalLine = 2;
  watchedFile.ranges = {{1, 2}};
  liveState.watchedFiles.push_back(watchedFile);
  threadManager_->writeAgentLiveState(threadId_, context_.identity.id, liveState);

  setFile("/work/sample.ts", "const a = 1;\nconst b = 2;\n");

  context_.history->turns.push_back(AgentTurn{});
  AgentTurn toolCallTurn;
  toolCallTurn.stopReason = StopReason::ToolUse;
  Message assistantMsg;
  assistantMsg.role = Role::Assistant;
  assistantMsg.content.push_back(
      ToolCallContent{"call-read", "file_read", R"({"path":"sample.ts"})"});
  toolCallTurn.messages.push_back(std::move(assistantMsg));
  context_.history->turns.push_back(std::move(toolCallTurn));

  AgentTurn toolResultTurn;
  Message toolMsg;
  toolMsg.role = Role::ToolResult;
  toolMsg.content.push_back(ToolResultContent{
      "call-read",
      R"({"line_start":1,"line_end":2,"lines_read":2,"watch_state":"updated","watch_scope":"full"})",
      true,
      "",
      ""});
  toolResultTurn.messages.push_back(std::move(toolMsg));
  context_.history->turns.push_back(std::move(toolResultTurn));

  const AgentHistory requestHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  ASSERT_GE(requestHistory.turns.size(), 1u);

  const AgentTurn& workTurn = requestHistory.turns.back();

  EXPECT_EQ(workTurn.turnId, "runtime-overlay-work-state");

  const std::string workText = firstTextContent(workTurn);
  EXPECT_NE(workText.find("Plan Title: Ship Runtime Overlay"), std::string::npos);
  EXPECT_NE(workText.find("#1 [Pending] Review live overlay content"),
            std::string::npos);

  const auto* toolResult = findToolResultByCallId(requestHistory, "call-read");
  ASSERT_NE(toolResult, nullptr);
  const std::string injectedContent = fileReadContentField(*toolResult);
  EXPECT_NE(injectedContent.find("<file path=\"/work/sample.ts\">"),
            std::string::npos);
  EXPECT_NE(injectedContent.find("1#"), std::string::npos);
  EXPECT_NE(injectedContent.find("const a = 1;"), std::string::npos);

  std::string allText;
  for (const auto& turn : requestHistory.turns) {
    allText += firstTextContent(turn);
    for (const auto& message : turn.messages) {
      for (const auto& part : message.content) {
        if (const auto* result = std::get_if<ToolResultContent>(&part)) {
          allText += fileReadContentField(*result);
        }
      }
    }
  }
  EXPECT_EQ(countOccurrences(allText, "<file path=\"/work/sample.ts\">"), 1u);
  EXPECT_EQ(allText.find("## WATCHED FILES"), std::string::npos);
}

TEST_F(RuntimeOverlayTest, FileReadReconciliationPersistsAndMergesWatchedCoverage) {
  context_.config.personaName = "worker";
  context_.identity.id = "worker-agent";

  setFile("/work/sample.ts",
          "one\n"
          "two\n"
          "three\n"
          "four\n"
          "five\n"
          "six\n");

  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "file_read", R"({"path":"sample.ts"})",
      R"({"line_start":1,"line_end":3,"read_full":false,"reached_end":false})");
  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "file_read", R"({"path":"sample.ts"})",
      R"({"line_start":4,"line_end":6,"read_full":false,"reached_end":true})");

  const AgentLiveState liveState =
      threadManager_->getAgentLiveState(threadId_, context_.identity.id);
  ASSERT_EQ(liveState.watchedFiles.size(), 1u);
  EXPECT_TRUE(liveState.watchedFiles.front().fullyRead);
  ASSERT_TRUE(liveState.watchedFiles.front().terminalLine.has_value());
  EXPECT_EQ(*liveState.watchedFiles.front().terminalLine, 6);

  context_.history->turns.push_back(AgentTurn{});
  AgentTurn toolCallTurn;
  toolCallTurn.stopReason = StopReason::ToolUse;
  Message assistantMsg;
  assistantMsg.role = Role::Assistant;
  assistantMsg.content.push_back(
      ToolCallContent{"call-read", "file_read", R"({"path":"sample.ts"})"});
  toolCallTurn.messages.push_back(std::move(assistantMsg));
  context_.history->turns.push_back(std::move(toolCallTurn));

  AgentTurn toolResultTurn;
  Message toolMsg;
  toolMsg.role = Role::ToolResult;
  toolMsg.content.push_back(ToolResultContent{
      "call-read",
      R"({"line_start":4,"line_end":6,"read_full":false,"reached_end":true})",
      true,
      "",
      ""});
  toolResultTurn.messages.push_back(std::move(toolMsg));
  context_.history->turns.push_back(std::move(toolResultTurn));

  const AgentHistory requestHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  const auto* toolResult = findToolResultByCallId(requestHistory, "call-read");
  ASSERT_NE(toolResult, nullptr);
  const std::string injectedContent = fileReadContentField(*toolResult);
  EXPECT_NE(injectedContent.find("six"), std::string::npos);
  EXPECT_NE(injectedContent.find("6#"), std::string::npos);
}

TEST_F(RuntimeOverlayTest, WatchedFilesOverlayFlagsExternalChangesSinceLastSync) {
  setFile("/work/sample.ts", "alpha\nbeta\n");

  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "file_read", R"({"path":"sample.ts"})",
      R"({"line_start":1,"line_end":2,"read_full":true,"reached_end":true})");

  context_.history->turns.push_back(AgentTurn{});
  AgentTurn toolCallTurn;
  toolCallTurn.stopReason = StopReason::ToolUse;
  Message assistantMsg;
  assistantMsg.role = Role::Assistant;
  assistantMsg.content.push_back(
      ToolCallContent{"call-read", "file_read", R"({"path":"sample.ts"})"});
  toolCallTurn.messages.push_back(std::move(assistantMsg));
  context_.history->turns.push_back(std::move(toolCallTurn));

  AgentTurn toolResultTurn;
  Message toolMsg;
  toolMsg.role = Role::ToolResult;
  toolMsg.content.push_back(ToolResultContent{
      "call-read",
      R"({"line_start":1,"line_end":2,"read_full":true,"reached_end":true})",
      true,
      "",
      ""});
  toolResultTurn.messages.push_back(std::move(toolMsg));
  context_.history->turns.push_back(std::move(toolResultTurn));

  const AgentHistory firstHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  const auto* firstResult = findToolResultByCallId(firstHistory, "call-read");
  ASSERT_NE(firstResult, nullptr);
  EXPECT_EQ(fileReadContentField(*firstResult).find("updated from disk since last sync"),
            std::string::npos);

  setFile("/work/sample.ts", "alpha\nbeta changed\n");

  const AgentHistory secondHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  const auto* secondResult = findToolResultByCallId(secondHistory, "call-read");
  ASSERT_NE(secondResult, nullptr);
  EXPECT_NE(fileReadContentField(*secondResult).find("updated from disk since last sync"),
            std::string::npos);
}

TEST_F(RuntimeOverlayTest, PartialWatchOverlayWarnsThatFullReadIsRequiredForEdits) {
  context_.config.personaName = "worker";
  context_.identity.id = "worker-agent";

  setFile("/work/sample.ts", "one\ntwo\nthree\nfour\n");

  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "file_read", R"({"path":"sample.ts"})",
      R"({"line_start":2,"line_end":3,"read_full":false,"reached_end":false})");

  context_.history->turns.push_back(AgentTurn{});
  AgentTurn toolCallTurn;
  toolCallTurn.stopReason = StopReason::ToolUse;
  Message assistantMsg;
  assistantMsg.role = Role::Assistant;
  assistantMsg.content.push_back(
      ToolCallContent{"call-read", "file_read", R"({"path":"sample.ts"})"});
  toolCallTurn.messages.push_back(std::move(assistantMsg));
  context_.history->turns.push_back(std::move(toolCallTurn));

  AgentTurn toolResultTurn;
  Message toolMsg;
  toolMsg.role = Role::ToolResult;
  toolMsg.content.push_back(ToolResultContent{
      "call-read",
      R"({"line_start":2,"line_end":3,"read_full":false,"reached_end":false})",
      true,
      "",
      ""});
  toolResultTurn.messages.push_back(std::move(toolMsg));
  context_.history->turns.push_back(std::move(toolResultTurn));

  const AgentHistory requestHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  const auto* toolResult = findToolResultByCallId(requestHistory, "call-read");
  ASSERT_NE(toolResult, nullptr);
  const std::string watchedText = fileReadContentField(*toolResult);
  EXPECT_NE(
      watchedText.find(
          "partial watch only; read the entire file before editing this file"),
      std::string::npos);
  EXPECT_NE(watchedText.find("2#"), std::string::npos);
  EXPECT_EQ(watchedText.find("1#"), std::string::npos);
}

TEST_F(RuntimeOverlayTest, RepeatedOverlappingReadsKeepSingleWatchedEntryPerFile) {
  context_.config.personaName = "worker";
  context_.identity.id = "worker-agent";

  setFile("/work/sample.ts",
          "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n");

  for (const char* resultJson : {
           R"({"line_start":1,"line_end":4,"read_full":false,"reached_end":false})",
           R"({"line_start":2,"line_end":5,"read_full":false,"reached_end":false})",
           R"({"line_start":3,"line_end":6,"read_full":false,"reached_end":false})",
           R"({"line_start":1,"line_end":6,"read_full":false,"reached_end":false})",
           R"({"line_start":4,"line_end":8,"read_full":false,"reached_end":false})",
           R"({"line_start":7,"line_end":10,"read_full":false,"reached_end":true})",
       }) {
    runtime_overlay::reconcileSuccessfulToolResult(
        context_, *host_, *workspace_, "file_read", R"({"path":"sample.ts"})",
        resultJson);
  }

  const AgentLiveState liveState =
      threadManager_->getAgentLiveState(threadId_, context_.identity.id);
  ASSERT_EQ(liveState.watchedFiles.size(), 1u);
  EXPECT_TRUE(liveState.watchedFiles.front().fullyRead);
  ASSERT_TRUE(liveState.watchedFiles.front().terminalLine.has_value());
  EXPECT_EQ(*liveState.watchedFiles.front().terminalLine, 10);

  context_.history->turns.push_back(AgentTurn{});
  AgentTurn toolCallTurn;
  toolCallTurn.stopReason = StopReason::ToolUse;
  Message assistantMsg;
  assistantMsg.role = Role::Assistant;
  assistantMsg.content.push_back(
      ToolCallContent{"call-read", "file_read", R"({"path":"sample.ts"})"});
  toolCallTurn.messages.push_back(std::move(assistantMsg));
  context_.history->turns.push_back(std::move(toolCallTurn));

  AgentTurn toolResultTurn;
  Message toolMsg;
  toolMsg.role = Role::ToolResult;
  toolMsg.content.push_back(ToolResultContent{
      "call-read",
      R"({"line_start":7,"line_end":10,"read_full":false,"reached_end":true})",
      true,
      "",
      ""});
  toolResultTurn.messages.push_back(std::move(toolMsg));
  context_.history->turns.push_back(std::move(toolResultTurn));

  const AgentHistory requestHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  const auto* toolResult = findToolResultByCallId(requestHistory, "call-read");
  ASSERT_NE(toolResult, nullptr);
  const std::string watchedText = fileReadContentField(*toolResult);
  EXPECT_EQ(countOccurrences(watchedText, "<file path=\"/work/sample.ts\">"), 1u);
  EXPECT_NE(watchedText.find("10#"), std::string::npos);
}

TEST_F(RuntimeOverlayTest, LatestFileReadResultReceivesWatchedContentExactlyOnce) {
  context_.config.personaName = "worker";
  context_.identity.id = "worker-agent";
  setFile("/work/first.ts", "alpha\nbeta\n");
  setFile("/work/second.ts", "one\ntwo\n");

  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "file_read", R"({"path":"first.ts"})",
      R"({"line_start":1,"line_end":2,"read_full":true,"reached_end":true})");
  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "file_read", R"({"path":"second.ts"})",
      R"({"line_start":1,"line_end":2,"read_full":true,"reached_end":true})");

  AgentTurn callOne;
  callOne.stopReason = StopReason::ToolUse;
  Message callOneMsg;
  callOneMsg.role = Role::Assistant;
  callOneMsg.content.push_back(
      ToolCallContent{"read-1", "file_read", R"({"path":"first.ts"})"});
  callOne.messages.push_back(std::move(callOneMsg));

  AgentTurn resultOne;
  Message resultOneMsg;
  resultOneMsg.role = Role::ToolResult;
  resultOneMsg.content.push_back(ToolResultContent{
      "read-1",
      R"({"line_start":1,"line_end":2,"read_full":true,"reached_end":true,"content":"stale"})",
      true,
      "",
      ""});
  resultOne.messages.push_back(std::move(resultOneMsg));

  AgentTurn callTwo;
  callTwo.stopReason = StopReason::ToolUse;
  Message callTwoMsg;
  callTwoMsg.role = Role::Assistant;
  callTwoMsg.content.push_back(
      ToolCallContent{"read-2", "file_read", R"({"path":"second.ts"})"});
  callTwo.messages.push_back(std::move(callTwoMsg));

  AgentTurn resultTwo;
  Message resultTwoMsg;
  resultTwoMsg.role = Role::ToolResult;
  resultTwoMsg.content.push_back(ToolResultContent{
      "read-2",
      R"({"line_start":1,"line_end":2,"read_full":true,"reached_end":true})",
      true,
      "",
      ""});
  resultTwo.messages.push_back(std::move(resultTwoMsg));

  context_.history->turns.push_back(std::move(callOne));
  context_.history->turns.push_back(std::move(resultOne));
  context_.history->turns.push_back(std::move(callTwo));
  context_.history->turns.push_back(std::move(resultTwo));

  const AgentHistory requestHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);

  const auto* firstResult = findToolResultByCallId(requestHistory, "read-1");
  const auto* secondResult = findToolResultByCallId(requestHistory, "read-2");
  ASSERT_NE(firstResult, nullptr);
  ASSERT_NE(secondResult, nullptr);

  EXPECT_TRUE(fileReadContentField(*firstResult).empty());
  EXPECT_EQ(firstResult->result.find("\"content\":\"stale\""), std::string::npos);
  const std::string latestContent = fileReadContentField(*secondResult);
  EXPECT_NE(latestContent.find("<file path=\"/work/first.ts\">"),
            std::string::npos);
  EXPECT_NE(latestContent.find("<file path=\"/work/second.ts\">"),
            std::string::npos);

  std::string allContext;
  for (const auto& turn : requestHistory.turns) {
    allContext += firstTextContent(turn);
    for (const auto& msg : turn.messages) {
      for (const auto& part : msg.content) {
        if (const auto* result = std::get_if<ToolResultContent>(&part)) {
          allContext += fileReadContentField(*result);
        }
      }
    }
  }
  EXPECT_EQ(countOccurrences(allContext, "<file path=\"/work/first.ts\">"), 1u);
  EXPECT_EQ(countOccurrences(allContext, "<file path=\"/work/second.ts\">"), 1u);
  EXPECT_EQ(allContext.find("## WATCHED FILES"), std::string::npos);
}

TEST_F(RuntimeOverlayTest, LatestFileEditResultReceivesUpdatedFilesAndMarksWorkspaceRead) {
  context_.config.personaName = "worker";
  context_.identity.id = "worker-agent";
  setFile("/work/sample.ts", "before\n");

  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "file_read", R"({"path":"sample.ts"})",
      R"({"line_start":1,"line_end":1,"read_full":true,"reached_end":true})");
  workspace_->recordFileEdit("/work/sample.ts");
  EXPECT_FALSE(workspace_->hasFullyReadFile("/work/sample.ts"));

  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "file_edit", R"({"path":"sample.ts"})",
      R"({"path":"sample.ts","mode":"hashline_edits","watch_state":"refreshed"})");
  EXPECT_TRUE(workspace_->hasFullyReadFile("/work/sample.ts"));

  AgentTurn readCall;
  readCall.stopReason = StopReason::ToolUse;
  Message readCallMsg;
  readCallMsg.role = Role::Assistant;
  readCallMsg.content.push_back(
      ToolCallContent{"read-1", "file_read", R"({"path":"sample.ts"})"});
  readCall.messages.push_back(std::move(readCallMsg));

  AgentTurn readResult;
  Message readResultMsg;
  readResultMsg.role = Role::ToolResult;
  readResultMsg.content.push_back(ToolResultContent{
      "read-1",
      R"({"line_start":1,"line_end":1,"read_full":true,"reached_end":true,"content":"stale"})",
      true,
      "",
      ""});
  readResult.messages.push_back(std::move(readResultMsg));

  AgentTurn editCall;
  editCall.stopReason = StopReason::ToolUse;
  Message editCallMsg;
  editCallMsg.role = Role::Assistant;
  editCallMsg.content.push_back(
      ToolCallContent{"edit-1", "file_edit", R"({"path":"sample.ts"})"});
  editCall.messages.push_back(std::move(editCallMsg));

  AgentTurn editResult;
  Message editResultMsg;
  editResultMsg.role = Role::ToolResult;
  editResultMsg.content.push_back(ToolResultContent{
      "edit-1",
      R"({"path":"sample.ts","mode":"hashline_edits","watch_state":"refreshed"})",
      true,
      "",
      ""});
  editResult.messages.push_back(std::move(editResultMsg));

  context_.history->turns.push_back(std::move(readCall));
  context_.history->turns.push_back(std::move(readResult));
  context_.history->turns.push_back(std::move(editCall));
  context_.history->turns.push_back(std::move(editResult));

  setFile("/work/sample.ts", "after\n");

  const AgentHistory requestHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);

  const auto* readResultPart = findToolResultByCallId(requestHistory, "read-1");
  const auto* editResultPart = findToolResultByCallId(requestHistory, "edit-1");
  ASSERT_NE(readResultPart, nullptr);
  ASSERT_NE(editResultPart, nullptr);

  EXPECT_TRUE(fileReadContentField(*readResultPart).empty());
  const std::string updatedFiles = fileEditUpdatedFilesField(*editResultPart);
  EXPECT_NE(updatedFiles.find("<file path=\"/work/sample.ts\">"),
            std::string::npos);
  EXPECT_NE(updatedFiles.find("after"), std::string::npos);
}

} // namespace
