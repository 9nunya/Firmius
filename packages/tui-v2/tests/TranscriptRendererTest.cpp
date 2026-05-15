#include "TranscriptRenderer.hpp"
#include "Terminal.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(TranscriptRendererTest, FormatUserMessage) {
  TranscriptLine line;
  line.kind = TranscriptLine::Kind::UserMessage;
  line.text = "Hello world";

  std::string formatted = TranscriptRenderer::formatLine(line, 80);
  // It should contain the prefix and the text
  EXPECT_NE(formatted.find("> Hello world"), std::string::npos);
}

TEST(TranscriptRendererTest, FormatToolCall) {
  TranscriptLine line;
  line.kind = TranscriptLine::Kind::ToolCall;
  line.text = "⚙ read_file";

  std::string formatted = TranscriptRenderer::formatLine(line, 80);
  EXPECT_NE(formatted.find("⚙ read_file"), std::string::npos);
}

TEST(TranscriptRendererTest, FormatToolResult) {
  TranscriptLine line;
  line.kind = TranscriptLine::Kind::ToolResult;
  line.text = "✓ read_file";
  line.success = true;

  std::string formatted = TranscriptRenderer::formatLine(line, 80);
  EXPECT_NE(formatted.find("✓ read_file"), std::string::npos);
}

TEST(TranscriptRendererTest, TurnsToLines) {
  std::vector<firmius::shared::AgentTurn> turns;
  firmius::shared::AgentTurn turn;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

  firmius::shared::Message msg;
  msg.role = firmius::shared::Role::User;

  firmius::shared::TextContent t;
  t.text = "Line 1\nLine 2";
  msg.content.push_back(t);

  turn.messages.push_back(msg);
  turns.push_back(turn);

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  auto lines = TranscriptRenderer::turnsToLines(turns, 80);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].text, "Line 1");
  EXPECT_EQ(lines[0].kind, TranscriptLine::Kind::UserMessage);
  EXPECT_EQ(lines[1].text, "Line 2");
  EXPECT_EQ(lines[1].kind, TranscriptLine::Kind::UserMessage);
}
