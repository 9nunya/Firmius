#include "TranscriptItem.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>
#include <memory>

using namespace firmius::tui2;

TEST(TranscriptRendererTest, RenderUserMessageItem) {
  UserMessageItem item("Hello world");
  auto lines = item.render(80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Hello world"), std::string::npos);
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
