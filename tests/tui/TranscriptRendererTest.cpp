#include "TranscriptItem.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include <gtest/gtest.h>
#include <memory>

using namespace firmius::tui;

TEST(TranscriptRendererTest, RenderUserMessageItem) {
  UserMessageItem item("Hello world");
  auto lines = item.render(80);
  ASSERT_GE(lines.size(), 1u);
  bool foundText = false;
  for (const auto& line : lines) {
    if (ansi::strip(line).find("Hello world") != std::string::npos) {
      foundText = true;
      break;
    }
  }
  EXPECT_TRUE(foundText);
}

TEST(TranscriptRendererTest, RenderToolCallItem) {
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"Execute","command":"ls"})");
  auto lines = item.render(80);
  ASSERT_GE(lines.size(), 1u);
  // Should render something (the presenter adds ANSI codes)
  EXPECT_FALSE(lines[0].empty());
}

TEST(TranscriptRendererTest, RenderToolCallFinished) {
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"Execute","command":"ls"})");
  item.setResult(true, R"({"exit_code":0,"duration_ms":123.4})");
  auto lines = item.render(80);
  ASSERT_GE(lines.size(), 1u);
  // Should contain checkmark or success indicator
  bool foundSuccess = false;
  for (const auto& line : lines) {
    if (line.find("\xe2\x9c\x93") != std::string::npos) foundSuccess = true;
  }
  EXPECT_TRUE(foundSuccess);
}

TEST(TranscriptRendererTest, ItemRowCountMatchesRender) {
  UserMessageItem item("test");
  auto lines = item.render(80);
  EXPECT_EQ(item.rowCount(80), static_cast<int>(lines.size()));
}

TEST(TranscriptRendererTest, StreamingTextRenders) {
  AgentTextItem item;
  item.appendDelta("Hello world\nLine 2");
  auto lines = item.render(80);
  EXPECT_GE(lines.size(), 2u);
}

// ── Horizontal rules ──
// Note: `---` after text is a setext h2 heading, not a horizontal rule.
// Use `***` or `___` for standalone horizontal rules, or `---` after blank line.

TEST(TranscriptRendererTest, HorizontalRuleStar) {
  AgentTextItem item;
  item.appendDelta("before\n\n***\n\nafter");
  item.finalize();
  auto lines = item.render(120);
  // before + empty + rule + empty + after = 5
  ASSERT_EQ(lines.size(), 5u);
  // Rule line should contain box-drawing ─
  EXPECT_NE(lines[2].find("\xe2\x94\x80"), std::string::npos);
}

TEST(TranscriptRendererTest, HorizontalRuleUnderscore) {
  AgentTextItem item;
  item.appendDelta("___");
  item.finalize();
  auto lines = item.render(120);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("\xe2\x94\x80"), std::string::npos);
}

TEST(TranscriptRendererTest, SetextH2NotConfusedWithHR) {
  // `---` after text is a setext h2, NOT a horizontal rule
  AgentTextItem item;
  item.appendDelta("Title\n---");
  item.finalize();
  auto lines = item.render(80);
  ASSERT_EQ(lines.size(), 1u);
  // Should be bold (heading), not a rule
  EXPECT_NE(lines[0].find("\x1b[1m"), std::string::npos);
}

// ── Numbered lists ──

TEST(TranscriptRendererTest, NumberedList) {
  AgentTextItem item;
  item.appendDelta("1. First\n2. Second\n3. Third");
  item.finalize();
  auto lines = item.render(80);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_NE(lines[0].find("1. "), std::string::npos);
  EXPECT_NE(lines[1].find("2. "), std::string::npos);
  EXPECT_NE(lines[2].find("3. "), std::string::npos);
}

// ── Task lists ──

TEST(TranscriptRendererTest, TaskListUnchecked) {
  AgentTextItem item;
  item.appendDelta("- [ ] Todo item");
  item.finalize();
  auto lines = item.render(80);
  ASSERT_EQ(lines.size(), 1u);
  // Should contain empty checkbox ☐
  EXPECT_NE(lines[0].find("\xe2\x98\x90"), std::string::npos);
}

TEST(TranscriptRendererTest, TaskListChecked) {
  AgentTextItem item;
  item.appendDelta("- [x] Done item");
  item.finalize();
  auto lines = item.render(80);
  ASSERT_EQ(lines.size(), 1u);
  // Should contain checked checkbox ☑
  EXPECT_NE(lines[0].find("\xe2\x98\x91"), std::string::npos);
}

// ── Tables ──

TEST(TranscriptRendererTest, BasicTable) {
  AgentTextItem item;
  item.appendDelta("| Name | Age |\n| --- | --- |\n| Alice | 30 |\n| Bob | 25 |");
  item.finalize();
  auto lines = item.render(80);
  // top + header + separator + 2 body rows + bottom = 6
  ASSERT_EQ(lines.size(), 6u);
  // Top border should have ┌
  EXPECT_NE(lines[0].find("\xe2\x94\x8c"), std::string::npos);
  // Bottom border should have ┘
  EXPECT_NE(lines[5].find("\xe2\x94\x98"), std::string::npos);
  // Header should contain "Name" and "Age"
  EXPECT_NE(lines[1].find("Name"), std::string::npos);
  EXPECT_NE(lines[1].find("Age"), std::string::npos);
}

TEST(TranscriptRendererTest, TableWithAlignment) {
  AgentTextItem item;
  item.appendDelta("| Left | Center | Right |\n| :--- | :---: | ---: |\n| a | b | c |");
  item.finalize();
  auto lines = item.render(80);
  ASSERT_EQ(lines.size(), 5u); // top + header + sep + 1 body + bottom
}

// ── Links ──

TEST(TranscriptRendererTest, InlineLink) {
  AgentTextItem item;
  item.appendDelta("Check [this](https://example.com) out");
  item.finalize();
  auto lines = item.render(80);
  ASSERT_GE(lines.size(), 1u);
  // Should contain the link text
  EXPECT_NE(lines[0].find("this"), std::string::npos);
  // Should contain the URL
  EXPECT_NE(lines[0].find("https://example.com"), std::string::npos);
}

// ── Setext headings ──

TEST(TranscriptRendererTest, SetextHeadingH1) {
  AgentTextItem item;
  item.appendDelta("Title\n===\nBody");
  item.finalize();
  auto lines = item.render(80);
  ASSERT_EQ(lines.size(), 2u); // title + body, underline skipped
  // Title should be bold (contains ANSI bold code)
  EXPECT_NE(lines[0].find("\x1b[1m"), std::string::npos);
}

TEST(TranscriptRendererTest, SetextHeadingH2) {
  AgentTextItem item;
  item.appendDelta("Subtitle\n---\nBody");
  item.finalize();
  auto lines = item.render(80);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_NE(lines[0].find("\x1b[1m"), std::string::npos);
}

// ── Language-tagged code fences ──

TEST(TranscriptRendererTest, LanguageTaggedFence) {
  AgentTextItem item;
  item.appendDelta("```python\nprint('hello')\n```");
  item.finalize();
  auto lines = item.render(80);
  // Fence lines are hidden, only code content is rendered
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("print('hello')"), std::string::npos);
}

// ── Mixed content ──

TEST(TranscriptRendererTest, MixedMarkdownContent) {
  AgentTextItem item;
  item.appendDelta("# Title\n\nSome **bold** text\n\n- Item 1\n- Item 2\n\n---\n\n1. First\n2. Second");
  item.finalize();
  auto lines = item.render(80);
  // title + empty + text + empty + bullet + bullet + empty + rule + empty + numbered + numbered
  ASSERT_GE(lines.size(), 9u);
}

TEST(TranscriptRendererTest, StreamingTablePartial) {
  AgentTextItem item;
  // Stream table in parts
  item.appendDelta("| A | B |\n| -");
  auto lines1 = item.render(80);
  // Incomplete table — separator not complete yet, renders as inline
  EXPECT_GE(lines1.size(), 1u);

  item.appendDelta(" | - |\n| 1 | 2 |");
  item.finalize();
  auto lines2 = item.render(80);
  // Now it's a complete table: top + header + sep + body + bottom = 5
  ASSERT_EQ(lines2.size(), 5u);
}
