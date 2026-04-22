#include "agents/RuntimeOverlay.hpp"
#include "agents/SkillLoader.hpp"
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

class RuntimeOverlaySecurityTest : public ::testing::Test {
protected:
  void SetUp() override {
    char tempTemplate[] = "/tmp/firmius_runtime_overlay_security_XXXXXX";
    char* result = mkdtemp(tempTemplate);
    ASSERT_NE(result, nullptr);
    tempDir_ = result;

    originalHome_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", tempDir_.c_str(), 1);

    // Set up skills directory
    skillsDir_ = tempDir_ + "/.agents/skills";
    std::filesystem::create_directories(skillsDir_);
    
    // Create a dummy skill
    std::filesystem::create_directories(skillsDir_ + "/test-skill");
    std::ofstream(skillsDir_ + "/test-skill/SKILL.md") << "name: Test Skill\n---\nBody";

    threadManager_ =
        std::make_unique<ThreadManager>(tempDir_ + "/.firmius/threads");
    ThreadMetadata metadata;
    metadata.title = "Runtime Overlay Security Test";
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

  std::string tempDir_;
  std::string originalHome_;
  std::string skillsDir_;
  std::string threadId_;
  std::unique_ptr<ThreadManager> threadManager_;
  AgentContext context_;
  std::unique_ptr<Workspace> workspace_;
  std::shared_ptr<firmius::test::MockHost> host_;
};

TEST_F(RuntimeOverlaySecurityTest, ReconcileSkillLoadRejectsUnauthorizedRoot) {
  // Tampered result with root at /
  const std::string resultJson = R"({"skill_id":"test-skill","path":"/work/test-agent.md","skill_root":"/"})";
  
  runtime_overlay::reconcileSuccessfulToolResult(context_, *host_, *workspace_,
                                                 "skill_load", R"({"id":"test-skill"})", resultJson);

  // Skill should be added, but path should NOT because the root is unauthorized
  EXPECT_EQ(context_.state.loadedSkills.size(), 1u);
  EXPECT_TRUE(context_.state.loadedAgentMds.empty());
  
  // But the root should NOT be added because it's unauthorized
  EXPECT_TRUE(context_.state.loadedSkillRoots.empty());
}

TEST_F(RuntimeOverlaySecurityTest, ReconcileSkillLoadAcceptsAuthorizedRoot) {
  // Valid result with root inside authorized skills dir
  std::string authorizedRoot = (std::filesystem::path(skillsDir_) / "test-skill").string();
  
  // Ensure it's normalized as it would be from the tool
  authorizedRoot = std::filesystem::path(authorizedRoot).lexically_normal().string();

  std::string resultJson = R"({"skill_id":"test-skill","path":"/work/test-agent.md","skill_root":")" + authorizedRoot + R"("})";
  
  runtime_overlay::reconcileSuccessfulToolResult(context_, *host_, *workspace_,
                                                 "skill_load", R"({"id":"test-skill"})", resultJson);

  EXPECT_EQ(context_.state.loadedSkills.size(), 1u);
  EXPECT_EQ(context_.state.loadedAgentMds.size(), 1u);
  
  // The root SHOULD be added because it's authorized
  ASSERT_EQ(context_.state.loadedSkillRoots.size(), 1u);
  EXPECT_EQ(context_.state.loadedSkillRoots["/work/test-agent.md"], authorizedRoot);
}

TEST_F(RuntimeOverlaySecurityTest, ReconstructStateFromHistoryRejectsUnauthorizedRoot) {
  AgentTurn turn1;
  Message msg1;
  msg1.role = Role::Assistant;
  msg1.content.push_back(ToolCallContent{"call-1", "skill_load", R"({"id":"test-skill"})"});
  turn1.messages.push_back(std::move(msg1));
  context_.history->turns.push_back(std::move(turn1));

  AgentTurn turn2;
  Message msg2;
  msg2.role = Role::ToolResult;
  // Tampered root at /
  msg2.content.push_back(ToolResultContent{"call-1", R"({"skill_id":"test-skill","path":"/work/test-skill.md","skill_root":"/"})", true, "", ""});
  turn2.messages.push_back(std::move(msg2));
  context_.history->turns.push_back(std::move(turn2));

  runtime_overlay::reconstructStateFromHistory(context_, *host_, *workspace_);

  // Skill should be added, but path should NOT because the root is unauthorized
  EXPECT_EQ(context_.state.loadedSkills.size(), 1u);
  EXPECT_TRUE(context_.state.loadedAgentMds.empty());
  
  // But root should be missing
  EXPECT_TRUE(context_.state.loadedSkillRoots.empty());
}

TEST_F(RuntimeOverlaySecurityTest, ReconstructStateFromHistoryAcceptsAuthorizedRoot) {
  std::string authorizedRoot = (std::filesystem::path(skillsDir_) / "test-skill").string();
  authorizedRoot = std::filesystem::path(authorizedRoot).lexically_normal().string();

  AgentTurn turn1;
  Message msg1;
  msg1.role = Role::Assistant;
  msg1.content.push_back(ToolCallContent{"call-1", "skill_load", R"({"id":"test-skill"})"});
  turn1.messages.push_back(std::move(msg1));
  context_.history->turns.push_back(std::move(turn1));

  AgentTurn turn2;
  Message msg2;
  msg2.role = Role::ToolResult;
  std::string resultJson = R"({"skill_id":"test-skill","path":"/work/test-skill.md","skill_root":")" + authorizedRoot + R"("})";
  msg2.content.push_back(ToolResultContent{"call-1", resultJson, true, "", ""});
  turn2.messages.push_back(std::move(msg2));
  context_.history->turns.push_back(std::move(turn2));

  runtime_overlay::reconstructStateFromHistory(context_, *host_, *workspace_);

  EXPECT_EQ(context_.state.loadedSkills.size(), 1u);
  EXPECT_EQ(context_.state.loadedAgentMds.size(), 1u);
  EXPECT_EQ(context_.state.loadedSkillRoots.size(), 1u);
  EXPECT_EQ(context_.state.loadedSkillRoots["/work/test-skill.md"], authorizedRoot);
}

TEST_F(RuntimeOverlaySecurityTest, ReconcileNonSkillToolDoesNotMutateLoadedMcpState) {
  context_.state.loadedMcpServers = {"existing"};
  context_.state.loadedMcpTools["existing"] = {"tool.persist"};

  const std::string resultJson =
      R"({"loaded_tools":["tool.alpha"],"loaded_resources":["res://alpha"],"loaded_prompts":["prompt.alpha"]})";

  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "mcp_load", R"({})", resultJson);

  ASSERT_EQ(context_.state.loadedMcpServers.size(), 1u);
  EXPECT_EQ(context_.state.loadedMcpServers[0], "existing");
  ASSERT_EQ(context_.state.loadedMcpTools.size(), 1u);
  EXPECT_EQ(context_.state.loadedMcpTools["existing"][0], "tool.persist");
  EXPECT_TRUE(context_.state.loadedMcpResources.empty());
  EXPECT_TRUE(context_.state.loadedMcpPrompts.empty());
}

TEST_F(RuntimeOverlaySecurityTest, ReconstructHistoryIgnoresLegacyMcpLoadCalls) {
  AgentTurn turn1;
  Message msg1;
  msg1.role = Role::Assistant;
  msg1.content.push_back(
      ToolCallContent{"call-mcp-bad", "mcp_load", R"({"server_name":"demo"})"});
  turn1.messages.push_back(std::move(msg1));
  context_.history->turns.push_back(std::move(turn1));

  AgentTurn turn2;
  Message msg2;
  msg2.role = Role::ToolResult;
  msg2.content.push_back(ToolResultContent{
      "call-mcp-bad",
      R"({"server_name":123,"loaded_tools":["tool.alpha"],"loaded_resources":["res://alpha"],"loaded_prompts":["prompt.alpha"]})",
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

TEST_F(RuntimeOverlaySecurityTest, BuildRequestHistoryOmitsLoadedMcpOverlayWhenNoStateWasAccepted) {
  runtime_overlay::reconcileSuccessfulToolResult(
      context_, *host_, *workspace_, "mcp_load", R"({})",
      R"({"loaded_tools":["tool.alpha"]})");

  const auto history = runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
      context_, *host_, *workspace_);

  for (const auto& turn : history.turns) {
    EXPECT_NE(turn.turnId, "runtime-overlay-loaded-mcp");
  }
}

} // namespace
