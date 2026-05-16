#include "TranscriptItem.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(TranscriptItemTest, UserMessageRendersWithPrefix) {
  UserMessageItem item("Hello world");
  auto lines = item.render(80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find(">"), std::string::npos);
  EXPECT_NE(lines[0].find("Hello world"), std::string::npos);
}

TEST(TranscriptItemTest, ErrorMessageRendersWithExclamation) {
  ErrorMessageItem item("Something went wrong");
  auto lines = item.render(80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("!"), std::string::npos);
}

TEST(TranscriptItemTest, SystemNoticeRendersDimmed) {
  SystemNoticeItem item("Notice text");
  auto lines = item.render(80);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_NE(lines[0].find("Notice text"), std::string::npos);
}

TEST(TranscriptItemTest, AgentTextAccumulatesAndFinalizes) {
  AgentTextItem item;
  EXPECT_TRUE(item.needsRender());

  item.appendDelta("Hello ");
  EXPECT_EQ(item.accumulated(), "Hello ");
  EXPECT_TRUE(item.needsRender());

  item.appendDelta("world\n");
  EXPECT_EQ(item.accumulated(), "Hello world\n");
  EXPECT_FALSE(item.isFinalized());

  item.finalize();
  EXPECT_TRUE(item.isFinalized());

  auto lines = item.render(80);
  EXPECT_GE(lines.size(), 1u);
}

TEST(TranscriptItemTest, AgentThinkingAccumulates) {
  AgentThinkingItem item;
  item.appendDelta("Hmm...");
  EXPECT_EQ(item.accumulated(), "Hmm...");
  EXPECT_FALSE(item.isFinalized());

  item.finalize();
  EXPECT_TRUE(item.isFinalized());
}

TEST(TranscriptItemTest, ToolCallPhaseTransitions) {
  ToolCallItem item("call-1", "Process", "agent-1");
  EXPECT_EQ(item.phase(), ToolPhase::Preparing);
  EXPECT_EQ(item.toolCallId(), "call-1");
  EXPECT_EQ(item.toolName(), "Process");

  item.setArgs(R"({"action":"Execute","command":"ls"})");
  item.setPhase(ToolPhase::Called);
  EXPECT_EQ(item.phase(), ToolPhase::Called);
  EXPECT_FALSE(item.args().empty());

  item.setResult(true, R"({"exit_code":0})");
  EXPECT_EQ(item.phase(), ToolPhase::FinishedSuccess);
  EXPECT_TRUE(item.success());
}

TEST(TranscriptItemTest, ToolCallDiffAttachment) {
  ToolCallItem item("call-1", "Edit", "agent-1");
  item.setArgs(R"({"patch":"..."})");

  firmius::shared::FileEditSignal signal;
  signal.path = "foo.cpp";
  signal.diffPreview = "--- a/foo.cpp\n+++ b/foo.cpp\n@@ -1,3 +1,4 @@\n line1\n+added\n line2";
  signal.addedLines = 1;
  signal.removedLines = 0;
  item.addDiffEdit(signal);

  EXPECT_EQ(item.diffEdits().size(), 1u);
  EXPECT_EQ(item.diffEdits()[0].path, "foo.cpp");
}

TEST(TranscriptItemTest, ToolCallExpandCollapse) {
  ToolCallItem item("call-1", "Process", "agent-1");
  EXPECT_FALSE(item.isExpanded());

  item.setExpanded(true);
  EXPECT_TRUE(item.isExpanded());
  EXPECT_TRUE(item.needsRender());

  item.markClean();
  item.setExpanded(false);
  EXPECT_FALSE(item.isExpanded());
  EXPECT_TRUE(item.needsRender());
}

TEST(TranscriptItemTest, ToolCallLiveState) {
  ToolCallItem item("call-1", "Process", "agent-1");
  item.setArgs(R"({"action":"Execute","command":"sleep 10"})");

  EXPECT_FALSE(item.isLive());
  item.setLive(true);
  EXPECT_TRUE(item.isLive());
  EXPECT_TRUE(item.needsRender());

  item.markClean();
  item.setLive(false);
  EXPECT_FALSE(item.isLive());
  EXPECT_TRUE(item.needsRender());
}

TEST(TranscriptItemTest, DirtyTracking) {
  UserMessageItem item("test");
  EXPECT_TRUE(item.needsRender());

  item.markClean();
  EXPECT_FALSE(item.needsRender());

  item.markDirty();
  EXPECT_TRUE(item.needsRender());
}

TEST(TranscriptItemTest, RowCountMatchesRender) {
  UserMessageItem item("Hello");
  auto lines = item.render(80);
  EXPECT_EQ(item.rowCount(80), static_cast<int>(lines.size()));
}
