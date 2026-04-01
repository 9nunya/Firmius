#include "utils/TerminalUtil.hpp"
#include <gtest/gtest.h>

using namespace firmius::shared;

class TerminalUtilTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(TerminalUtilTest, EscapeSequenceNewline) {
    // Literal backslash-n should be translated to actual newline
    std::string input = "Hello\\nWorld";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\nWorld");
}

TEST_F(TerminalUtilTest, EscapeSequenceTab) {
    // Literal backslash-t should be translated to actual tab
    std::string input = "Hello\\tWorld";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\tWorld");
}

TEST_F(TerminalUtilTest, EscapeSequenceCarriageReturn) {
    // Literal backslash-r should be translated to actual carriage return
    std::string input = "Hello\\rWorld";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\rWorld");
}

TEST_F(TerminalUtilTest, EscapeSequenceBackslash) {
    // Double backslash should be translated to single backslash
    std::string input = "Hello\\\\World";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\\World");
}

TEST_F(TerminalUtilTest, EscapeSequenceQuote) {
    // Backslash-quote should be translated to quote
    std::string input = "Hello\\\"World\\\"";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\"World\"");
}

TEST_F(TerminalUtilTest, MultipleEscapeSequences) {
    // Multiple escape sequences in one string
    std::string input = "Line1\\nLine2\\tTabbed\\nLine3";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Line1\nLine2\tTabbed\nLine3");
}

TEST_F(TerminalUtilTest, ControlTagEnter) {
    // {Enter} tag should be translated to newline
    std::string input = "Hello{Enter}World";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\nWorld");
}

TEST_F(TerminalUtilTest, ControlTagTab) {
    // {Tab} tag should be translated to tab
    std::string input = "Hello{Tab}World";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\tWorld");
}

TEST_F(TerminalUtilTest, ControlTagCtrlC) {
    // {Ctrl+C} should be translated to ASCII 3 (ETX)
    std::string input = "{Ctrl+C}";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], '\x03');
}

TEST_F(TerminalUtilTest, ControlTagCtrlD) {
    // {Ctrl+D} should be translated to ASCII 4 (EOT)
    std::string input = "{Ctrl+D}";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], '\x04');
}

TEST_F(TerminalUtilTest, ControlTagCtrlZ) {
    // {Ctrl+Z} should be translated to ASCII 26 (SUB)
    std::string input = "{Ctrl+Z}";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], '\x1a');
}

TEST_F(TerminalUtilTest, ControlTagAltA) {
    // {Alt+A} should be translated to ESC + 'A'
    std::string input = "{Alt+A}";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], '\x1b');
    EXPECT_EQ(result[1], 'A');
}

TEST_F(TerminalUtilTest, ControlTagF1) {
    // {F1} should be translated to function key sequence
    std::string input = "{F1}";
    std::string result = TerminalUtil::translate(input);
    EXPECT_FALSE(result.empty());
    // F1 produces escape sequence starting with ESC
    EXPECT_EQ(result[0], '\x1b');
}

TEST_F(TerminalUtilTest, MixedEscapeSequencesAndControlTags) {
    // Mix of escape sequences and control tags
    std::string input = "Hello\\n{Enter}World\\t{Tab}End";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\n\nWorld\t\tEnd");
}

TEST_F(TerminalUtilTest, LiteralNewlinePreserved) {
    // Actual newlines in input should be preserved
    std::string input = "Hello\nWorld";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\nWorld");
}

TEST_F(TerminalUtilTest, EmptyString) {
    std::string input = "";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "");
}

TEST_F(TerminalUtilTest, NoEscapeSequences) {
    std::string input = "Hello World";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello World");
}

TEST_F(TerminalUtilTest, TrailingBackslash) {
    // Trailing backslash without escape char should be preserved
    std::string input = "Hello\\";
    std::string result = TerminalUtil::translate(input);
    EXPECT_EQ(result, "Hello\\");
}

TEST_F(TerminalUtilTest, ArrowKeys) {
    // Arrow key tags
    EXPECT_EQ(TerminalUtil::translate("{Up}"), "\x1b[A");
    EXPECT_EQ(TerminalUtil::translate("{Down}"), "\x1b[B");
    EXPECT_EQ(TerminalUtil::translate("{Right}"), "\x1b[C");
    EXPECT_EQ(TerminalUtil::translate("{Left}"), "\x1b[D");
}

TEST_F(TerminalUtilTest, SpecialKeys) {
    // Special key tags
    EXPECT_EQ(TerminalUtil::translate("{Esc}"), "\x1b");
    EXPECT_EQ(TerminalUtil::translate("{Backspace}"), "\x7f");
    EXPECT_EQ(TerminalUtil::translate("{Home}"), "\x1b[H");
    EXPECT_EQ(TerminalUtil::translate("{End}"), "\x1b[F");
}
