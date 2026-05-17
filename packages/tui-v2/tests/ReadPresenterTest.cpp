#include "tools/ReadPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(ReadPresenterTest, MatchesRead) {
  ReadPresenter p;
  EXPECT_TRUE(p.matches("Read"));
  EXPECT_FALSE(p.matches("Edit"));
  EXPECT_FALSE(p.matches("Grep"));
}

TEST(ReadPresenterTest, ReadCalled) {
  ReadPresenter p;
  ToolCallItem item("call-1", "Read", "agent-1");
  item.setArgs(R"({"path":"foo.cpp","start_line":1,"end_line":50})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Reading"), std::string::npos);
}

TEST(ReadPresenterTest, ReadFinished) {
  ReadPresenter p;
  ToolCallItem item("call-1", "Read", "agent-1");
  item.setArgs(R"({"path":"foo.cpp","start_line":1,"end_line":50})");
  item.setResult(true, R"({"lines_read":50})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("lines 1-50"), std::string::npos);
}
