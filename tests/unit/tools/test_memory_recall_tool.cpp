#include "../mocks/MockAgent.hpp"
#include "../mocks/MockHost.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"
#include "tools/MemoryRecallTool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

class MemoryRecallToolTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char *existingHome = std::getenv("HOME");
    if (existingHome) {
      originalHome_ = existingHome;
    }
    tempHome_ = std::filesystem::temp_directory_path() /
                ("firmius_memory_recall_" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
    std::filesystem::create_directories(tempHome_);
    setenv("HOME", tempHome_.c_str(), 1);
    std::filesystem::create_directories(tempHome_ / ".firmius" / "threads");
    tm_ = std::make_unique<ThreadManager>(
        (tempHome_ / ".firmius" / "threads").string());

    ThreadMetadata metadata;
    metadata.title = "Recall";
    metadata.hostOptions.type = HostType::Local;
    metadata.cwd = "/work";
    metadata.leadPersona = "lead";
    threadId_ = tm_->createThread(metadata);
    agentId_ = "agent-1";

    context_.history = std::make_shared<AgentHistory>();
    context_.history->threadId = threadId_;
    context_.identity.id = agentId_;
    agent_ = std::make_unique<firmius::test::MockAgent>(context_);
    host_ = std::make_unique<firmius::test::MockHost>();
  }

  void TearDown() override {
    if (originalHome_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", originalHome_.c_str(), 1);
    }
    std::filesystem::remove_all(tempHome_);
  }

  AgentTurn makeTurn(const std::string &turnId, Role role,
                     const std::string &text) {
    AgentTurn turn;
    turn.turnId = turnId;
    Message msg;
    msg.id = "msg-" + turnId;
    msg.role = role;
    msg.timestamp = 1;
    msg.content.push_back(TextContent{text});
    turn.messages.push_back(std::move(msg));
    return turn;
  }

  void seedHistory(const std::vector<AgentTurn> &turns) {
    Journaler journaler(threadId_, agentId_);
    for (const auto &turn : turns) {
      journaler.appendTurn(turn);
    }
  }

  ToolContext makeToolContext() {
    return ToolContext{*host_, *agent_, "call-1", nullptr, nullptr};
  }

  std::filesystem::path tempHome_;
  std::string originalHome_;
  std::unique_ptr<ThreadManager> tm_;
  std::string threadId_;
  std::string agentId_;
  AgentContext context_;
  std::unique_ptr<firmius::test::MockAgent> agent_;
  std::unique_ptr<firmius::test::MockHost> host_;
};

TEST_F(MemoryRecallToolTest, FailsWithoutActiveThread) {
  MemoryRecallTool tool;
  agent_->getMutableContext().history = std::make_shared<AgentHistory>();
  auto ctx = makeToolContext();
  auto result = tool.execute(MemoryRecallInput{}, ctx);
  EXPECT_FALSE(result.success);
}

TEST_F(MemoryRecallToolTest, FailsWhenNoHistoryExists) {
  MemoryRecallTool tool;
  auto ctx = makeToolContext();
  auto result = tool.execute(MemoryRecallInput{}, ctx);
  EXPECT_FALSE(result.success);
}

TEST_F(MemoryRecallToolTest, DefaultReturnsLastTurns) {
  seedHistory({makeTurn("t1", Role::User, "one"), makeTurn("t2", Role::Assistant, "two"),
               makeTurn("t3", Role::User, "three")});
  MemoryRecallTool tool;
  auto ctx = makeToolContext();
  auto result = tool.execute(MemoryRecallInput{}, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("t3"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, RangeRecallUsesStartAndEndTurnIds) {
  seedHistory({makeTurn("t1", Role::User, "one"), makeTurn("t2", Role::Assistant, "two"),
               makeTurn("t3", Role::User, "three")});
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.start_turn_id = "t1";
  input.end_turn_id = "t2";
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("t1"), std::string::npos);
  EXPECT_NE(result.data.find("t2"), std::string::npos);
  EXPECT_EQ(result.data.find("t3"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, RangeRecallFailsForUnknownStart) {
  seedHistory({makeTurn("t1", Role::User, "one")});
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.start_turn_id = "missing";
  input.end_turn_id = "t1";
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
}

TEST_F(MemoryRecallToolTest, RangeRecallFailsForUnknownEnd) {
  seedHistory({makeTurn("t1", Role::User, "one")});
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.start_turn_id = "t1";
  input.end_turn_id = "missing";
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
}

TEST_F(MemoryRecallToolTest, RangeRecallFailsWhenStartComesAfterEnd) {
  seedHistory({makeTurn("t1", Role::User, "one"), makeTurn("t2", Role::User, "two")});
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.start_turn_id = "t2";
  input.end_turn_id = "t1";
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
}

TEST_F(MemoryRecallToolTest, CursorRecallReturnsPageAroundCursor) {
  seedHistory({makeTurn("t1", Role::User, "one"), makeTurn("t2", Role::Assistant, "two"),
               makeTurn("t3", Role::User, "three"), makeTurn("t4", Role::Assistant, "four")});
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.cursor_turn_id = "t2";
  input.page = 0;
  input.page_size = 2;
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("t2"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, CursorRecallPageMovesForward) {
  seedHistory({makeTurn("t1", Role::User, "one"), makeTurn("t2", Role::Assistant, "two"),
               makeTurn("t3", Role::User, "three"), makeTurn("t4", Role::Assistant, "four")});
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.cursor_turn_id = "t1";
  input.page = 1;
  input.page_size = 2;
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("t3"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, CursorRecallFailsForUnknownCursor) {
  seedHistory({makeTurn("t1", Role::User, "one")});
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.cursor_turn_id = "missing";
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
}

TEST_F(MemoryRecallToolTest, IncludeSystemFalseHidesSystemTurns) {
  seedHistory({makeTurn("s1", Role::System, "hidden"), makeTurn("u1", Role::User, "visible")});
  MemoryRecallTool tool;
  auto ctx = makeToolContext();
  auto result = tool.execute(MemoryRecallInput{}, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.data.find("hidden"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, IncludeSystemTrueShowsSystemTurns) {
  seedHistory({makeTurn("s1", Role::System, "shown"), makeTurn("u1", Role::User, "visible")});
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.include_system = true;
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("shown"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, PageSizeIsClampedToMinimum) {
  seedHistory({makeTurn("t1", Role::User, "one"), makeTurn("t2", Role::User, "two")});
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.page_size = 0;
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("\"startIndex\":1"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, PageSizeIsClampedToMaximum) {
  std::vector<AgentTurn> turns;
  for (int i = 0; i < 70; ++i) {
    turns.push_back(makeTurn("t" + std::to_string(i), Role::User, "turn"));
  }
  seedHistory(turns);
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.page_size = 200;
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("\"startIndex\":6"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, RendersToolCallContent) {
  AgentTurn turn;
  turn.turnId = "t1";
  Message msg;
  msg.role = Role::Assistant;
  msg.timestamp = 1;
  msg.content.push_back(ToolCallContent{"call-1", "file_read", "{\"path\":\"a\"}"});
  turn.messages.push_back(msg);
  seedHistory({turn});
  MemoryRecallTool tool;
  auto ctx = makeToolContext();
  auto result = tool.execute(MemoryRecallInput{}, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("tool_call"), std::string::npos);
  EXPECT_NE(result.data.find("file_read"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, RendersToolResultContent) {
  AgentTurn turn;
  turn.turnId = "t1";
  Message msg;
  msg.role = Role::ToolResult;
  msg.timestamp = 1;
  msg.content.push_back(ToolResultContent{"call-1", "{\"ok\":true}", true, "", ""});
  turn.messages.push_back(msg);
  seedHistory({turn});
  MemoryRecallTool tool;
  auto ctx = makeToolContext();
  auto result = tool.execute(MemoryRecallInput{}, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("tool_result"), std::string::npos);
  EXPECT_NE(result.data.find("ok"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, RendersThinkingContent) {
  AgentTurn turn;
  turn.turnId = "t1";
  Message msg;
  msg.role = Role::Assistant;
  msg.timestamp = 1;
  msg.content.push_back(ThinkingContent{"reasoning", ""});
  turn.messages.push_back(msg);
  seedHistory({turn});
  MemoryRecallTool tool;
  auto ctx = makeToolContext();
  auto result = tool.execute(MemoryRecallInput{}, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("<thinking>reasoning</thinking>"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, RendersNoticeContent) {
  AgentTurn turn;
  turn.turnId = "t1";
  Message msg;
  msg.role = Role::System;
  msg.timestamp = 1;
  msg.content.push_back(NoticeContent{"title", "message", "details", NoticeSeverity::Info});
  turn.messages.push_back(msg);
  seedHistory({turn});
  MemoryRecallTool tool;
  MemoryRecallInput input;
  input.include_system = true;
  auto ctx = makeToolContext();
  auto result = tool.execute(input, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("title: message"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, RendersErrorContent) {
  AgentTurn turn;
  turn.turnId = "t1";
  Message msg;
  msg.role = Role::Error;
  msg.timestamp = 1;
  msg.content.push_back(ErrorContent{"Provider Error", "boom", "details"});
  turn.messages.push_back(msg);
  seedHistory({turn});
  MemoryRecallTool tool;
  auto ctx = makeToolContext();
  auto result = tool.execute(MemoryRecallInput{}, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("Provider Error: boom"), std::string::npos);
}

TEST_F(MemoryRecallToolTest, ReturnsThreadAndAgentMetadata) {
  seedHistory({makeTurn("t1", Role::User, "one")});
  MemoryRecallTool tool;
  auto ctx = makeToolContext();
  auto result = tool.execute(MemoryRecallInput{}, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find(threadId_), std::string::npos);
  EXPECT_NE(result.data.find(agentId_), std::string::npos);
}

} // namespace
