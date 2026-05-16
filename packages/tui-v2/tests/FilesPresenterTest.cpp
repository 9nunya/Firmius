#include "tools/FilesPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(FilesPresenterTest, MatchesFiles) {
  FilesPresenter p;
  EXPECT_TRUE(p.matches("Files"));
  EXPECT_FALSE(p.matches("Edit"));
}

TEST(FilesPresenterTest, ReadCalled) {
  FilesPresenter p;
  ToolCallItem item("call-1", "Files", "agent-1");
  item.setArgs(R"({"action":"Read","path":"foo.cpp","start_line":1,"end_line":50})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Reading"), std::string::npos);
}

TEST(FilesPresenterTest, ReadFinished) {
  FilesPresenter p;
  ToolCallItem item("call-1", "Files", "agent-1");
  item.setArgs(R"({"action":"Read","path":"foo.cpp","start_line":1,"end_line":50})");
  item.setResult(true, R"({"lines_read":50})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("lines 1-50"), std::string::npos);
}

TEST(FilesPresenterTest, ListFinished) {
  FilesPresenter p;
  ToolCallItem item("call-1", "Files", "agent-1");
  item.setArgs(R"({"action":"List","path":"."})");
  item.setResult(true, R"({"count":42})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("42"), std::string::npos);
}

TEST(FilesPresenterTest, GrepFinished) {
  FilesPresenter p;
  ToolCallItem item("call-1", "Files", "agent-1");
  item.setArgs(R"({"action":"Grep","path":".","pattern":"TODO"})");
  item.setResult(true, R"({"count":15})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("15"), std::string::npos);
}

TEST(FilesPresenterTest, GrepBudgetHit) {
  FilesPresenter p;
  ToolCallItem item("call-1", "Files", "agent-1");
  item.setArgs(R"({"action":"Grep","path":".","pattern":"TODO"})");
  item.setResult(true, R"({"count":100,"budget_hit":true})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("budget hit"), std::string::npos);
}

TEST(FilesPresenterTest, GlobFinished) {
  FilesPresenter p;
  ToolCallItem item("call-1", "Files", "agent-1");
  item.setArgs(R"({"action":"Glob","path":".","glob":"*.cpp"})");
  item.setResult(true, R"({"count":8})");
  auto lines = p.render(item, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("8"), std::string::npos);
}
