#include "tools/LspPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(LspPresenterTest, MatchesLsp) {
  LspPresenter p;
  EXPECT_TRUE(p.matches("Lsp"));
  EXPECT_FALSE(p.matches("Edit"));
}

TEST(LspPresenterTest, QueryCalled) {
  LspPresenter p;
  ToolCallItem item("call-1", "Lsp", "agent-1");
  item.setArgs(R"({"action":"Query","operation":"hover","path":"foo.cpp","line":10,"character":5})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("hover"), std::string::npos);
  EXPECT_NE(lines[0].find("foo.cpp"), std::string::npos);
}

TEST(LspPresenterTest, DiagnosticsFinished) {
  LspPresenter p;
  ToolCallItem item("call-1", "Lsp", "agent-1");
  item.setArgs(R"({"action":"Diagnostics","path":"foo.cpp"})");
  item.setResult(true, R"({"errors":2,"warnings":5})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("2"), std::string::npos);
  EXPECT_NE(lines[0].find("5"), std::string::npos);
}

TEST(LspPresenterTest, HoverFinished) {
  LspPresenter p;
  ToolCallItem item("call-1", "Lsp", "agent-1");
  item.setArgs(R"({"action":"Query","operation":"hover","path":"foo.cpp","line":10,"character":5})");
  item.setResult(true, R"json({"value":"int foo(int x)"})json");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("int foo"), std::string::npos);
}
