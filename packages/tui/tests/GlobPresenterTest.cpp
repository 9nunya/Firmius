#include "tools/GlobPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(GlobPresenterTest, MatchesGlob) {
  GlobPresenter p;
  EXPECT_TRUE(p.matches("Glob"));
  EXPECT_FALSE(p.matches("Grep"));
}

TEST(GlobPresenterTest, GlobCalled) {
  GlobPresenter p;
  ToolCallItem item("call-1", "Glob", "agent-1");
  item.setArgs(R"({"path":".","glob":"*.cpp"})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Glob"), std::string::npos);
}

TEST(GlobPresenterTest, GlobFinished) {
  GlobPresenter p;
  ToolCallItem item("call-1", "Glob", "agent-1");
  item.setArgs(R"({"path":".","glob":"*.cpp"})");
  item.setResult(true, R"({"count":8})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("8"), std::string::npos);
}

TEST(GlobPresenterTest, GlobBudgetHit) {
  GlobPresenter p;
  ToolCallItem item("call-1", "Glob", "agent-1");
  item.setArgs(R"({"path":".","glob":"*.cpp"})");
  item.setResult(true, R"({"count":100,"budget_hit":true})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("budget hit"), std::string::npos);
}
