#include "agents/RuntimeOverlay.hpp"
#include "environment/Workspace.hpp"
#include "persistence/ThreadManager.hpp"
#include "../mocks/MockHost.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

  AgentTurn makeTurn(const std::string& turnId, Role role,
                     const std::string& text) {
    AgentTurn turn;
    turn.turnId = turnId;
    Message msg;
    msg.role = role;
    msg.timestamp = 1;
    msg.content.push_back(TextContent{text});
    turn.messages.push_back(std::move(msg));
    return turn;
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

  const AgentTurn *workTurn = nullptr;
  for (const auto &turn : requestHistory.turns) {
    if (turn.turnId == "runtime-overlay-work-state") {
      workTurn = &turn;
      break;
    }
  }
  ASSERT_NE(workTurn, nullptr);

  const std::string workText = firstTextContent(*workTurn);
  EXPECT_FALSE(workText.empty());
  EXPECT_NE(workText.find("Review live overlay content"), std::string::npos);

}


TEST_F(RuntimeOverlayTest, BuildRequestHistoryAppendsWorkerState) {
  context_.config.personaName = "ember";
  context_.identity.id = "worker-agent";

  const auto history =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  ASSERT_GE(history.turns.size(), 1u);

  const AgentTurn *workTurn = nullptr;
  for (const auto &turn : history.turns) {
    if (turn.turnId == "runtime-overlay-work-state") {
      workTurn = &turn;
      break;
    }
  }
  ASSERT_NE(workTurn, nullptr);
  const std::string workText = firstTextContent(*workTurn);
  EXPECT_NE(workText.find("Role: ember"), std::string::npos);
}

TEST_F(RuntimeOverlayTest, BuildRequestHistoryAppendsExecutorState) {
  context_.config.personaName = "forge";
  context_.identity.id = "executor-agent";

  const auto history =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  ASSERT_GE(history.turns.size(), 1u);

  const AgentTurn *workTurn = nullptr;
  for (const auto &turn : history.turns) {
    if (turn.turnId == "runtime-overlay-work-state") {
      workTurn = &turn;
      break;
    }
  }
  ASSERT_NE(workTurn, nullptr);
  const std::string workText = firstTextContent(*workTurn);
  EXPECT_NE(workText.find("Role: forge"), std::string::npos);
}

TEST_F(RuntimeOverlayTest, BuildRequestHistoryAppendsUserMemoryOverlay) {
  const std::filesystem::path userRoot =
      std::filesystem::path(tempDir_) / ".firmius" / "user";
  std::filesystem::create_directories(userRoot);
  {
    std::ofstream out(userRoot / "USER.md");
    out << "# USER\n\n- Prefers concise summaries.\n";
  }
  {
    std::ofstream out(userRoot / "BEHAVIOR.md");
    out << "# BEHAVIOR\n\n- Run tests after code changes.\n";
  }

  const auto history = runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
      context_, *host_, *workspace_);

  bool foundUserMemory = false;
  for (const auto &turn : history.turns) {
    if (turn.turnId != "runtime-overlay-user-memory") {
      continue;
    }
    foundUserMemory = true;
    const std::string text = firstTextContent(turn);
    EXPECT_NE(text.find("## USER MEMORY"), std::string::npos);
    EXPECT_NE(text.find("Prefers concise summaries"), std::string::npos);
    EXPECT_NE(text.find("Run tests after code changes"), std::string::npos);
  }
  EXPECT_TRUE(foundUserMemory);
}

TEST_F(RuntimeOverlayTest, BuildRequestHistorySkipsUserMemoryOverlayForBenchmarkThreads) {
  auto metadata = threadManager_->getMetadata(threadId_);
  metadata.isBenchmarkRun = true;
  metadata.benchmarkId = "swebench";
  threadManager_->updateMetadata(threadId_, metadata);

  const std::filesystem::path userRoot =
      std::filesystem::path(tempDir_) / ".firmius" / "user";
  std::filesystem::create_directories(userRoot);
  {
    std::ofstream out(userRoot / "USER.md");
    out << "# USER\n\n- Should not appear.\n";
  }

  const auto history = runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
      context_, *host_, *workspace_);

  for (const auto &turn : history.turns) {
    EXPECT_NE(turn.turnId, "runtime-overlay-user-memory");
  }
}

} // namespace

TEST_F(RuntimeOverlayTest, ReconcileSkillLoadUpdatesState) {
  std::string skillsDir = tempDir_ + "/.agents/skills";
  std::filesystem::create_directories(skillsDir);
  setenv("FIRMIUS_SKILLS_DIR", skillsDir.c_str(), 1);

  const std::string skillRoot = skillsDir + "/test-skill";
  std::filesystem::create_directories(skillRoot);
  const std::string path = skillRoot + "/test-agent.md";

  // 1. Valid authorized path
  const std::string validResultJson = R"({"skill_id":"test-skill","path":")" + path + R"(","skill_root":")" + skillRoot + R"("})";
  runtime_overlay::reconcileSuccessfulToolResult(context_, *host_, *workspace_,
                                                 "skill_load", R"({"id":"test-skill"})", validResultJson);

  EXPECT_EQ(context_.state.loadedSkills.size(), 1u);
  EXPECT_EQ(context_.state.loadedSkills[0], "test-skill");
  EXPECT_EQ(context_.state.loadedAgentMds.size(), 1u);
  EXPECT_EQ(context_.state.loadedAgentMds[0], path);
  EXPECT_EQ(context_.state.loadedSkillRoots[path], skillRoot);

  // 2. Unauthorized path (outside skills dir)
  const std::string invalidPath = "/tmp/malicious.md";
  const std::string invalidResultJson = R"({"skill_id":"evil-skill","path":")" + invalidPath + R"(","skill_root":"/tmp"})";
  runtime_overlay::reconcileSuccessfulToolResult(context_, *host_, *workspace_,
                                                 "skill_load", R"({"id":"evil-skill"})", invalidResultJson);

  // Should have loaded the skill ID, but NOT the path/root if it's untrusted
  EXPECT_EQ(context_.state.loadedSkills.size(), 2u);
  EXPECT_EQ(context_.state.loadedAgentMds.size(), 1u); // still only the first one
  EXPECT_EQ(context_.state.loadedSkillRoots.size(), 1u);
}

TEST_F(RuntimeOverlayTest, BuildRequestHistoryAppendsLoadedSkillsOverlay) {
  context_.state.loadedSkills = {"skill-a", "skill-b"};
  context_.state.loadedAgentMds = {"/work/agent-x.md"};
  setFile("/work/agent-x.md", "Agent X Instructions\n");
  context_.state.loadedSkillRoots["/work/agent-x.md"] = "/";

  const auto history =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);
  
  // Should have work state overlay AND loaded skills overlay
  ASSERT_GE(history.turns.size(), 2u);

  bool foundSkills = false;
  for (const auto& turn : history.turns) {
    if (turn.turnId == "runtime-overlay-loaded-skills") {
      foundSkills = true;
      const std::string text = firstTextContent(turn);
      EXPECT_NE(text.find("## LOADED SKILLS"), std::string::npos);
      EXPECT_NE(text.find("- skill-a"), std::string::npos);
      EXPECT_NE(text.find("- skill-b"), std::string::npos);
      EXPECT_NE(text.find("### /work/agent-x.md"), std::string::npos);
      EXPECT_NE(text.find("Agent X Instructions"), std::string::npos);
    }
  }
  EXPECT_TRUE(foundSkills);

}

TEST_F(RuntimeOverlayTest, BuildLoadedSkillsOverlayFailsClosedOnMissingRoot) {
  context_.state.loadedAgentMds = {"/work/untracked.md"};
  setFile("/work/untracked.md", "Secret instructions\n");

  const auto history =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);

  bool foundSkills = false;
  for (const auto& turn : history.turns) {
    if (turn.turnId == "runtime-overlay-loaded-skills") {
      foundSkills = true;
      const std::string text = firstTextContent(turn);
      EXPECT_EQ(text.find("### /work/untracked.md"), std::string::npos);
      EXPECT_EQ(text.find("(unavailable: missing recorded root)"), std::string::npos);
      EXPECT_EQ(text.find("Secret instructions"), std::string::npos);
    }
  }
  EXPECT_TRUE(foundSkills);
}

TEST_F(RuntimeOverlayTest, BuildLoadedSkillsOverlayFailsOnSecurityViolation) {
  context_.state.loadedAgentMds = {"/etc/passwd"};
  context_.state.loadedSkillRoots["/etc/passwd"] = "/work";
  // MockHost might not care, but FSUtil::isCanonicalSubpath will.
  setFile("/etc/passwd", "root:x:0:0:root:/root:/bin/bash\n");

  const auto history =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);

  bool foundSkills = false;
  for (const auto& turn : history.turns) {
    if (turn.turnId == "runtime-overlay-loaded-skills") {
      foundSkills = true;
      const std::string text = firstTextContent(turn);
      EXPECT_EQ(text.find("### /etc/passwd"), std::string::npos);
      EXPECT_EQ(text.find("(unavailable: security violation)"), std::string::npos);
      EXPECT_EQ(text.find("root:x:0:0"), std::string::npos);
    }
  }
  EXPECT_TRUE(foundSkills);
}

TEST_F(RuntimeOverlayTest, ReconstructStateFromHistoryRecoversLoadedSkills) {
  std::string skillsDir = tempDir_ + "/.agents/skills";
  std::filesystem::create_directories(skillsDir);
  setenv("FIRMIUS_SKILLS_DIR", skillsDir.c_str(), 1);

  const std::string skillRoot = skillsDir + "/test-skill";
  std::filesystem::create_directories(skillRoot);
  const std::string path = skillRoot + "/test-skill.md";

  AgentTurn turn1;
  Message msg1;
  msg1.role = Role::Assistant;
  msg1.content.push_back(ToolCallContent{"call-1", "skill_load", R"({"what":"test-skill"})"});
  turn1.messages.push_back(std::move(msg1));
  context_.history->turns.push_back(std::move(turn1));

  AgentTurn turn2;
  Message msg2;
  msg2.role = Role::ToolResult;
  msg2.content.push_back(ToolResultContent{"call-1", R"({"skill_id":"test-skill","path":")" + path + R"(","skill_root":")" + skillRoot + R"("})", true, "", ""});
  turn2.messages.push_back(std::move(msg2));
  context_.history->turns.push_back(std::move(turn2));

  // State should be empty initially
  EXPECT_TRUE(context_.state.loadedSkills.empty());
  EXPECT_TRUE(context_.state.loadedAgentMds.empty());

  runtime_overlay::reconstructStateFromHistory(context_, *host_, *workspace_);

  // State should be reconstructed
  EXPECT_EQ(context_.state.loadedSkills.size(), 1u);
  EXPECT_EQ(context_.state.loadedSkills[0], "test-skill");
  EXPECT_EQ(context_.state.loadedAgentMds.size(), 1u);
  EXPECT_EQ(context_.state.loadedAgentMds[0], path);
}

TEST_F(RuntimeOverlayTest, ReconcileLegacyMcpLoadNoLongerUpdatesState) {
  const std::string resultJson =
      R"({"server_name":"demo","loaded_tools":["tool.alpha","tool.beta"],"loaded_resources":["res://alpha"],"loaded_prompts":["prompt.alpha"]})";

  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "mcp_load", R"({"server_name":"demo"})",
      resultJson);

  EXPECT_TRUE(context_.state.loadedMcpServers.empty());
  EXPECT_TRUE(context_.state.loadedMcpTools.empty());
  EXPECT_TRUE(context_.state.loadedMcpResources.empty());
  EXPECT_TRUE(context_.state.loadedMcpPrompts.empty());
}

TEST_F(RuntimeOverlayTest, ReconstructStateFromHistoryIgnoresLegacyMcpLoadState) {
  AgentTurn turn1;
  Message msg1;
  msg1.role = Role::Assistant;
  msg1.content.push_back(
      ToolCallContent{"call-mcp-1", "mcp_load", R"({"server_name":"demo"})"});
  turn1.messages.push_back(std::move(msg1));
  context_.history->turns.push_back(std::move(turn1));

  AgentTurn turn2;
  Message msg2;
  msg2.role = Role::ToolResult;
  msg2.content.push_back(ToolResultContent{
      "call-mcp-1",
      R"({"server_name":"demo","loaded_tools":["tool.alpha"],"loaded_resources":["res://alpha"],"loaded_prompts":["prompt.alpha"]})",
      true,
      "",
      ""});
  turn2.messages.push_back(std::move(msg2));
  context_.history->turns.push_back(std::move(turn2));

  runtime_overlay::reconstructStateFromHistory(context_, *host_, *workspace_);

  EXPECT_TRUE(context_.state.loadedMcpServers.empty());
  EXPECT_TRUE(context_.state.loadedMcpTools.empty());
  EXPECT_TRUE(context_.state.loadedMcpResources.empty());
  EXPECT_TRUE(context_.state.loadedMcpPrompts.empty());
}

TEST_F(RuntimeOverlayTest, BuildRequestHistoryOmitsLoadedMcpSummaryWhenEmpty) {
  const auto history =
      runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
          context_, *host_, *workspace_);

  for (const auto& turn : history.turns) {
    EXPECT_NE(turn.turnId, "runtime-overlay-loaded-mcp");
  }
}
