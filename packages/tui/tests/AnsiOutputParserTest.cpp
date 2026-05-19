#include "AnsiOutputParser.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(AnsiOutputParserTest, PlainText) {
  auto lines = AnsiOutputParser::toLines("hello world", 80);
  EXPECT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("hello world"), std::string::npos);
}

TEST(AnsiOutputParserTest, MultilineText) {
  auto lines = AnsiOutputParser::toLines("line1\nline2\nline3", 80);
  EXPECT_EQ(lines.size(), 3u);
}

TEST(AnsiOutputParserTest, MaxLinesTruncation) {
  auto lines = AnsiOutputParser::toLines("a\nb\nc\nd\ne", 80, 3);
  // Should show last 3 lines + an "earlier lines" notice
  EXPECT_EQ(lines.size(), 4u);
  EXPECT_NE(lines[0].find("earlier lines"), std::string::npos);
}

TEST(AnsiOutputParserTest, EmptyInput) {
  auto lines = AnsiOutputParser::toLines("", 80);
  EXPECT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("no output"), std::string::npos);
}

TEST(AnsiOutputParserTest, LinePrefix) {
  auto lines = AnsiOutputParser::toLines("test", 80);
  // Should have the bar prefix
  EXPECT_NE(lines[0].find("\xe2\x94\x82"), std::string::npos);
}

TEST(AnsiOutputParserTest, CarriageReturnHandling) {
  auto lines = AnsiOutputParser::toLines("line1\r\nline2", 80);
  EXPECT_EQ(lines.size(), 2u);
}
