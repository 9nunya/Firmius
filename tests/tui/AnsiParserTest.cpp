#include "AnsiParser.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui;

TEST(AnsiParserTest, PlainText) {
  auto cells = AnsiParser::parse("hello");
  ASSERT_EQ(cells.size(), 5u);
  EXPECT_EQ(cells[0].ch, U'h');
  EXPECT_EQ(cells[4].ch, U'o');
  // Default style
  EXPECT_EQ(cells[0].fg.type, CellColor::Type::Default);
  EXPECT_FALSE(cells[0].style.bold);
}

TEST(AnsiParserTest, BoldText) {
  auto cells = AnsiParser::parse("\x1b[1mhi\x1b[22m");
  ASSERT_EQ(cells.size(), 2u);
  EXPECT_TRUE(cells[0].style.bold);
  EXPECT_TRUE(cells[1].style.bold);
}

TEST(AnsiParserTest, FgColor) {
  auto cells = AnsiParser::parse("\x1b[31mR\x1b[39m");
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_EQ(cells[0].fg.type, CellColor::Type::Palette256);
  EXPECT_EQ(cells[0].fg.index, 1); // 31 = red = index 1
}

TEST(AnsiParserTest, FgRgb) {
  auto cells = AnsiParser::parse("\x1b[38;2;100;200;50mX");
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_EQ(cells[0].fg.type, CellColor::Type::RGB);
  EXPECT_EQ(cells[0].fg.r, 100);
  EXPECT_EQ(cells[0].fg.g, 200);
  EXPECT_EQ(cells[0].fg.b, 50);
}

TEST(AnsiParserTest, BgRgb) {
  auto cells = AnsiParser::parse("\x1b[48;2;10;20;30mZ");
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_EQ(cells[0].bg.type, CellColor::Type::RGB);
  EXPECT_EQ(cells[0].bg.r, 10);
  EXPECT_EQ(cells[0].bg.g, 20);
  EXPECT_EQ(cells[0].bg.b, 30);
}

TEST(AnsiParserTest, Fg256) {
  auto cells = AnsiParser::parse("\x1b[38;5;196m!");
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_EQ(cells[0].fg.type, CellColor::Type::Palette256);
  EXPECT_EQ(cells[0].fg.index, 196);
}

TEST(AnsiParserTest, ResetClearsAll) {
  auto cells = AnsiParser::parse("\x1b[1;3;4;31mX\x1b[0mY");
  ASSERT_EQ(cells.size(), 2u);
  // X has styles
  EXPECT_TRUE(cells[0].style.bold);
  EXPECT_TRUE(cells[0].style.italic);
  EXPECT_TRUE(cells[0].style.underline);
  EXPECT_EQ(cells[0].fg.type, CellColor::Type::Palette256);
  // Y is reset
  EXPECT_FALSE(cells[1].style.bold);
  EXPECT_FALSE(cells[1].style.italic);
  EXPECT_EQ(cells[1].fg.type, CellColor::Type::Default);
}

TEST(AnsiParserTest, NestedStyles) {
  // Bold + fg green
  auto cells = AnsiParser::parse("\x1b[1;32mAB");
  ASSERT_EQ(cells.size(), 2u);
  EXPECT_TRUE(cells[0].style.bold);
  EXPECT_EQ(cells[0].fg.index, 2); // 32 = green
  EXPECT_TRUE(cells[1].style.bold);
  EXPECT_EQ(cells[1].fg.index, 2);
}

TEST(AnsiParserTest, MaxWidthTruncation) {
  auto cells = AnsiParser::parse("abcdefghij", 5);
  ASSERT_EQ(cells.size(), 5u);
  EXPECT_EQ(cells[0].ch, U'a');
  EXPECT_EQ(cells[4].ch, U'e');
}

TEST(AnsiParserTest, Utf8TwoByte) {
  // U+00E9 = e-acute = C3 A9 in UTF-8
  auto cells = AnsiParser::parse("\xC3\xA9");
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_EQ(cells[0].ch, U'\u00E9');
}

TEST(AnsiParserTest, Utf8ThreeByte) {
  // U+20AC = Euro sign = E2 82 AC
  auto cells = AnsiParser::parse("\xE2\x82\xAC");
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_EQ(cells[0].ch, U'\u20AC');
}

TEST(AnsiParserTest, Utf8FourByte) {
  // U+1F600 = grinning face = F0 9F 98 80
  auto cells = AnsiParser::parse("\xF0\x9F\x98\x80");
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_EQ(cells[0].ch, U'\U0001F600');
}

TEST(AnsiParserTest, MixedAnsiAndText) {
  auto cells = AnsiParser::parse("\x1b[1mBold\x1b[22m Normal");
  ASSERT_EQ(cells.size(), 11u); // "Bold Normal" = 10 chars + space
  EXPECT_TRUE(cells[0].style.bold);
  EXPECT_TRUE(cells[3].style.bold);  // 'd' of "Bold"
  EXPECT_FALSE(cells[4].style.bold); // ' ' space after reset
}

TEST(AnsiParserTest, EmptyString) {
  auto cells = AnsiParser::parse("");
  EXPECT_TRUE(cells.empty());
}

TEST(AnsiParserTest, CursorMovementIgnored) {
  // CUU (cursor up) should be ignored, text still appears
  auto cells = AnsiParser::parse("\x1b[3Ahi");
  ASSERT_EQ(cells.size(), 2u);
  EXPECT_EQ(cells[0].ch, U'h');
  EXPECT_EQ(cells[1].ch, U'i');
}

TEST(AnsiParserTest, DimAndStrikethrough) {
  auto cells = AnsiParser::parse("\x1b[2;9mX");
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_TRUE(cells[0].style.dim);
  EXPECT_TRUE(cells[0].style.strikethrough);
}

TEST(AnsiParserTest, Invert) {
  auto cells = AnsiParser::parse("\x1b[7mI");
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_TRUE(cells[0].style.invert);
}
