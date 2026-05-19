#include "tools/WebPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(WebPresenterTest, MatchesWeb) {
  WebPresenter p;
  EXPECT_TRUE(p.matches("Web"));
  EXPECT_FALSE(p.matches("Edit"));
}

TEST(WebPresenterTest, SearchCalled) {
  WebPresenter p;
  ToolCallItem item("call-1", "Web", "agent-1");
  item.setArgs(R"({"action":"Search","query":"how to C++"})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("how to C++"), std::string::npos);
}

TEST(WebPresenterTest, SearchFinished) {
  WebPresenter p;
  ToolCallItem item("call-1", "Web", "agent-1");
  item.setArgs(R"({"action":"Search","query":"test"})");
  item.setResult(true, R"({"results":[{"url":"http://example.com","title":"Example"}],"provider":"google"})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("1"), std::string::npos);
  EXPECT_NE(lines[0].find("google"), std::string::npos);
}

TEST(WebPresenterTest, FetchFinished) {
  WebPresenter p;
  ToolCallItem item("call-1", "Web", "agent-1");
  item.setArgs(R"({"action":"Fetch","url":"http://example.com"})");
  item.setResult(true, R"({"size":10240})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("10.0 KB"), std::string::npos);
}

TEST(WebPresenterTest, FetchRedirected) {
  WebPresenter p;
  ToolCallItem item("call-1", "Web", "agent-1");
  item.setArgs(R"({"action":"Fetch","url":"http://example.com"})");
  item.setResult(true, R"({"redirected_to":"/tmp/page.html"})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("/tmp/page.html"), std::string::npos);
}
