#include "tools/ListPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(ListPresenterTest, MatchesList) {
  ListPresenter p;
  EXPECT_TRUE(p.matches("List"));
  EXPECT_FALSE(p.matches("Read"));
}

TEST(ListPresenterTest, ListCalled) {
  ListPresenter p;
  ToolCallItem item("call-1", "List", "agent-1");
  item.setArgs(R"({"path":"."})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Listing"), std::string::npos);
}

TEST(ListPresenterTest, ListFinished) {
  ListPresenter p;
  ToolCallItem item("call-1", "List", "agent-1");
  item.setArgs(R"({"path":"."})");
  item.setResult(true, R"({"count":42})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("42"), std::string::npos);
}
