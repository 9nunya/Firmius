#include "tools/TodoPresenter.hpp"
#include "tools/ModeSwitchPresenter.hpp"
#include "tools/SkillPresenter.hpp"
#include "tools/McpPresenter.hpp"
#include "tools/GenericPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

// ── Todo ──

TEST(TodoPresenterTest, MatchesTodo) {
  TodoPresenter p;
  EXPECT_TRUE(p.matches("Todo"));
  EXPECT_FALSE(p.matches("Edit"));
}

TEST(TodoPresenterTest, CalledPhase) {
  TodoPresenter p;
  ToolCallItem item("call-1", "Todo", "agent-1");
  item.setArgs(R"({"patch":"[ ] task 1\n[*] task 2\n[x] task 3"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
}

TEST(TodoPresenterTest, FinishedSuccess) {
  TodoPresenter p;
  ToolCallItem item("call-1", "Todo", "agent-1");
  item.setArgs(R"({"patch":"..."})");
  item.setResult(true, R"({"items":[{"status":"done","text":"task 1"},{"status":"pending","text":"task 2"}]})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 2u);
}

// ── ModeSwitch ──

TEST(ModeSwitchPresenterTest, MatchesModeSwitch) {
  ModeSwitchPresenter p;
  EXPECT_TRUE(p.matches("ModeSwitch"));
  EXPECT_FALSE(p.matches("Edit"));
}

TEST(ModeSwitchPresenterTest, CalledWithName) {
  ModeSwitchPresenter p;
  ToolCallItem item("call-1", "ModeSwitch", "agent-1");
  item.setArgs(R"({"name":"forge"})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("forge"), std::string::npos);
}

TEST(ModeSwitchPresenterTest, CalledClearMode) {
  ModeSwitchPresenter p;
  ToolCallItem item("call-1", "ModeSwitch", "agent-1");
  item.setArgs(R"({"name":""})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Clearing"), std::string::npos);
}

TEST(ModeSwitchPresenterTest, Finished) {
  ModeSwitchPresenter p;
  ToolCallItem item("call-1", "ModeSwitch", "agent-1");
  item.setArgs(R"({"name":"forge"})");
  item.setResult(true, R"({"to_mode":"forge"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("forge"), std::string::npos);
}

// ── Skill ──

TEST(SkillPresenterTest, MatchesSkill) {
  SkillPresenter p;
  EXPECT_TRUE(p.matches("Skill"));
  EXPECT_FALSE(p.matches("Edit"));
}

TEST(SkillPresenterTest, FinishedSuccess) {
  SkillPresenter p;
  ToolCallItem item("call-1", "Skill", "agent-1");
  item.setArgs(R"({"what":"pdf"})");
  item.setResult(true, R"({"name":"pdf","description":"PDF reader"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("pdf"), std::string::npos);
}

// ── MCP ──

TEST(McpPresenterTest, MatchesMcpTools) {
  McpPresenter p;
  EXPECT_TRUE(p.matches("mcp__github__search"));
  EXPECT_TRUE(p.matches("mcp__jira__create_issue"));
  EXPECT_FALSE(p.matches("Edit"));
}

TEST(McpPresenterTest, CalledPhase) {
  McpPresenter p;
  ToolCallItem item("call-1", "mcp__github__search", "agent-1");
  item.setArgs(R"({"query":"test","limit":10})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 2u);
  EXPECT_NE(lines[0].find("github"), std::string::npos);
  EXPECT_NE(lines[0].find("search"), std::string::npos);
}

TEST(McpPresenterTest, FinishedSuccess) {
  McpPresenter p;
  ToolCallItem item("call-1", "mcp__github__search", "agent-1");
  item.setArgs(R"({"query":"test"})");
  item.setResult(true, R"({"results":[]})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("done"), std::string::npos);
}

// ── Generic ──

TEST(GenericPresenterTest, MatchesEverything) {
  GenericPresenter p;
  EXPECT_TRUE(p.matches("Anything"));
  EXPECT_TRUE(p.matches(""));
  EXPECT_TRUE(p.matches("UnknownTool"));
}

TEST(GenericPresenterTest, CalledPhase) {
  GenericPresenter p;
  ToolCallItem item("call-1", "SomeTool", "agent-1");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("SomeTool"), std::string::npos);
}

TEST(GenericPresenterTest, FinishedSuccess) {
  GenericPresenter p;
  ToolCallItem item("call-1", "SomeTool", "agent-1");
  item.setResult(true, R"({"result":"ok"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("done"), std::string::npos);
}

TEST(GenericPresenterTest, FinishedError) {
  GenericPresenter p;
  ToolCallItem item("call-1", "SomeTool", "agent-1");
  item.setResult(false, R"({"error":"failed"})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("failed"), std::string::npos);
}
