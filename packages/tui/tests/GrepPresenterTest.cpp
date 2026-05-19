#include "tools/GrepPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(GrepPresenterTest, MatchesGrep) {
  GrepPresenter p;
  EXPECT_TRUE(p.matches("Grep"));
  EXPECT_FALSE(p.matches("Glob"));
}

TEST(GrepPresenterTest, GrepCalled) {
  GrepPresenter p;
  ToolCallItem item("call-1", "Grep", "agent-1");
  item.setArgs(R"({"path":".","pattern":"TODO"})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Grep"), std::string::npos);
}

TEST(GrepPresenterTest, GrepFinished) {
  GrepPresenter p;
  ToolCallItem item("call-1", "Grep", "agent-1");
  item.setArgs(R"({"path":".","pattern":"TODO"})");
  item.setResult(true, R"({"count":15})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("15"), std::string::npos);
}

TEST(GrepPresenterTest, GrepBudgetHit) {
  GrepPresenter p;
  ToolCallItem item("call-1", "Grep", "agent-1");
  item.setArgs(R"({"path":".","pattern":"TODO"})");
  item.setResult(true, R"({"count":100,"budget_hit":true})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("budget hit"), std::string::npos);
}
