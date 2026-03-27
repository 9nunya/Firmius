#include "agents/RuntimeOverlay.hpp"
#include "environment/Workspace.hpp"
#include "persistence/ThreadManager.hpp"
#include "../mocks/MockHost.hpp"

#include <gtest/gtest.h>

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

TEST_F(RuntimeOverlayTest, BuildRequestHistoryAppendsLeadStateAndWatchedFiles) {
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

  const AgentHistory requestHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  ASSERT_GE(requestHistory.turns.size(), 2u);

  const AgentTurn& workTurn = requestHistory.turns[requestHistory.turns.size() - 2];
  const AgentTurn& watchedTurn = requestHistory.turns.back();

  EXPECT_EQ(workTurn.turnId, "runtime-overlay-work-state");
  EXPECT_EQ(watchedTurn.turnId, "runtime-overlay-watched-files");

  const std::string workText = firstTextContent(workTurn);
  const std::string watchedText = firstTextContent(watchedTurn);
  EXPECT_NE(workText.find("Plan Title: Ship Runtime Overlay"), std::string::npos);
  EXPECT_NE(workText.find("#1 [Pending] Review live overlay content"),
            std::string::npos);
  EXPECT_NE(watchedText.find("<file path=\"/work/sample.ts\">"),
            std::string::npos);
  EXPECT_NE(watchedText.find("1#"), std::string::npos);
  EXPECT_NE(watchedText.find("const a = 1;"), std::string::npos);
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

  const AgentHistory requestHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  const std::string watchedText = firstTextContent(requestHistory.turns.back());
  EXPECT_NE(watchedText.find("six"), std::string::npos);
  EXPECT_NE(watchedText.find("6#"), std::string::npos);
}

TEST_F(RuntimeOverlayTest, WatchedFilesOverlayFlagsExternalChangesSinceLastSync) {
  setFile("/work/sample.ts", "alpha\nbeta\n");

  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "file_read", R"({"path":"sample.ts"})",
      R"({"line_start":1,"line_end":2,"read_full":true,"reached_end":true})");

  const AgentHistory firstHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  const std::string firstWatched = firstTextContent(firstHistory.turns.back());
  EXPECT_EQ(firstWatched.find("updated from disk since last sync"),
            std::string::npos);

  setFile("/work/sample.ts", "alpha\nbeta changed\n");

  const AgentHistory secondHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  const std::string secondWatched = firstTextContent(secondHistory.turns.back());
  EXPECT_NE(secondWatched.find("updated from disk since last sync"),
            std::string::npos);
}

TEST_F(RuntimeOverlayTest, PartialWatchOverlayWarnsThatFullReadIsRequiredForEdits) {
  context_.config.personaName = "worker";
  context_.identity.id = "worker-agent";

  setFile("/work/sample.ts", "one\ntwo\nthree\nfour\n");

  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "file_read", R"({"path":"sample.ts"})",
      R"({"line_start":2,"line_end":3,"read_full":false,"reached_end":false})");

  const AgentHistory requestHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  const std::string watchedText = firstTextContent(requestHistory.turns.back());
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

  const AgentHistory requestHistory =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  const std::string watchedText = firstTextContent(requestHistory.turns.back());
  EXPECT_EQ(countOccurrences(watchedText, "<file path=\"/work/sample.ts\">"), 1u);
  EXPECT_NE(watchedText.find("10#"), std::string::npos);
}

} // namespace
