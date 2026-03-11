#include <gtest/gtest.h>
#include "components/ANSIParser.hpp"
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

class ANSIParserTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(ANSIParserTest, ParsePlainTextColor) {
  std::string text = "Hello World";
  auto element = ParseANSI(text);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(ANSIParserTest, ParseBoldText) {
  std::string text = "Hello \x1b[1mBold\x1b[0m World";
  auto element = ParseANSI(text);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(ANSIParserTest, ParseColoredText) {
  std::string text = "\x1b[31mRed\x1b[0m \x1b[32mGreen\x1b[0m \x1b[34mBlue\x1b[0m";
  auto element = ParseANSI(text);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(ANSIParserTest, ParseMultipleStyles) {
  std::string text = "\x1b[1;31mBold Red\x1b[0m \x1b[4;32mUnderline Green\x1b[0m";
  auto element = ParseANSI(text);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(ANSIParserTest, ParseLines) {
  std::string text = "Line 1\nLine 2\nLine 3";
  auto lines = ParseANSILines(text);
  EXPECT_EQ(lines.size(), 3u);
}

TEST_F(ANSIParserTest, ParseLinesWithTrailingNewline) {
  std::string text = "Line 1\nLine 2\n";
  auto lines = ParseANSILines(text);
  EXPECT_EQ(lines.size(), 3u); // Including empty last line
}

TEST_F(ANSIParserTest, ParseEmptyString) {
  std::string text = "";
  auto element = ParseANSI(text);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(ANSIParserTest, ParseOnlyEscapeCodes) {
  std::string text = "\x1b[0m\x1b[1m\x1b[31m";
  auto element = ParseANSI(text);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(ANSIParserTest, ParseBrightColors) {
  std::string text = "\x1b[91mBright Red\x1b[0m \x1b[95mBright Magenta\x1b[0m";
  auto element = ParseANSI(text);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(ANSIParserTest, ParseBackgroundColors) {
  std::string text = "\x1b[44mBlue Background\x1b[0m";
  auto element = ParseANSI(text);
  EXPECT_TRUE(element != nullptr);
}

TEST_F(ANSIParserTest, ParseComplexTerminalOutput) {
  // Simulate real terminal output with multiple styles
  std::string text = 
    "\x1b[32m✓\x1b[0m Build succeeded\n"
    "\x1b[33m⚠\x1b[0m \x1b[1mWarning:\x1b[0m Deprecated function\n"
    "\x1b[31m✗\x1b[0m Test failed: assertion error";
  
  auto lines = ParseANSILines(text);
  EXPECT_EQ(lines.size(), 3u);
}

} // namespace firmius::tui
