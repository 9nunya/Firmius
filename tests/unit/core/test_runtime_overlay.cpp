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

}


TEST_F(RuntimeOverlayTest, BuildRequestHistoryAppendsWorkerState) {
  context_.config.personaName = "worker";
  context_.identity.id = "worker-agent";

  const auto history =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  ASSERT_GE(history.turns.size(), 1u);

  const std::string workText = firstTextContent(history.turns.back());
  EXPECT_NE(workText.find("Role: worker"), std::string::npos);
}

TEST_F(RuntimeOverlayTest, BuildRequestHistoryAppendsExecutorState) {
  context_.config.personaName = "executor";
  context_.identity.id = "executor-agent";

  const auto history =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  ASSERT_GE(history.turns.size(), 1u);

  const std::string workText = firstTextContent(history.turns.back());
  EXPECT_NE(workText.find("Role: executor"), std::string::npos);
}
} // namespace
