#include <gtest/gtest.h>

#include "MarkdownParser.hpp"
#include <ftxui/screen/screen.hpp>
#include <ftxui/dom/elements.hpp>

using namespace firmius::tui;

class MarkdownParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_ = std::make_unique<MarkdownParser>();
    }

    void TearDown() override {
        parser_.reset();
    }

    std::vector<ftxui::Element> Parse(const std::string& text) {
        auto elements = parser_->parseChunk(text);
        if (elements.empty()) {
            auto finished = parser_->finish();
            elements.insert(elements.end(), std::make_move_iterator(finished.begin()),
                           std::make_move_iterator(finished.end()));
        }
        return elements;
    }

    std::unique_ptr<MarkdownParser> parser_;
};

TEST_F(MarkdownParserTest, ParseSimpleBold) {
    auto elements = Parse("**bold text**\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseBoldInSentence) {
    auto elements = Parse("This is **bold** text.\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseMultipleBoldSections) {
    auto elements = Parse("**first** and **second** bold\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseUnclosedBold) {
    auto elements = Parse("This **is unclosed\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseSimpleCode) {
    auto elements = Parse("`code snippet`\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseCodeInSentence) {
    auto elements = Parse("Use the `print()` function.\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseMultipleCodeSections) {
    auto elements = Parse("`first` and `second` code\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseUnclosedCode) {
    auto elements = Parse("This `is unclosed\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseSimpleFence) {
    auto elements = parser_->parseChunk("```\ncode line\n```\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseFenceWithLanguage) {
    auto elements = parser_->parseChunk("```cpp\nint main() {\n  return 0;\n}\n```\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseFenceWithMultipleLines) {
    auto elements = parser_->parseChunk("```python\ndef hello():\n    print('world')\n    return 42\n```\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseUnclosedFence) {
    auto elements = parser_->parseChunk("```\nunclosed code\n");
    EXPECT_EQ(elements.size(), 0u);
    auto remaining = parser_->finish();
    EXPECT_EQ(remaining.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseEmptyFence) {
    auto elements = parser_->parseChunk("```\n```\n");
    EXPECT_EQ(elements.size(), 0u);
}

TEST_F(MarkdownParserTest, StreamPartialBold) {
    auto part1 = parser_->parseChunk("This is **par");
    EXPECT_EQ(part1.size(), 0u);

    auto part2 = parser_->parseChunk("tial** text\n");
    EXPECT_EQ(part2.size(), 0u);

    auto finished = parser_->finish();
    EXPECT_EQ(finished.size(), 1u);
}

TEST_F(MarkdownParserTest, StreamPartialCode) {
    auto part1 = parser_->parseChunk("Use `print");
    EXPECT_EQ(part1.size(), 0u);

    auto part2 = parser_->parseChunk("()` function\n");
    EXPECT_EQ(part2.size(), 0u);

    auto finished = parser_->finish();
    EXPECT_EQ(finished.size(), 1u);
}

TEST_F(MarkdownParserTest, StreamPartialFence) {
    auto part1 = parser_->parseChunk("```cpp\nint");
    EXPECT_EQ(part1.size(), 0u);

    auto part2 = parser_->parseChunk(" main();\n```\n");
    EXPECT_EQ(part2.size(), 1u);
}

TEST_F(MarkdownParserTest, StreamMultiChunkParagraph) {
    auto part1 = parser_->parseChunk("First line");
    EXPECT_EQ(part1.size(), 0u);

    auto part2 = parser_->parseChunk(" second part");
    EXPECT_EQ(part2.size(), 0u);

    auto part3 = parser_->parseChunk(" third part\n");
    EXPECT_EQ(part3.size(), 0u);

    auto finished = parser_->finish();
    EXPECT_EQ(finished.size(), 1u);
}

TEST_F(MarkdownParserTest, StripDoneTagSimple) {
    auto elements = Parse("Hello world<done />\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, StripDoneTagNoSpace) {
    auto elements = Parse("Hello world<done/>\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, StripDoneTagExtraSpace) {
    auto elements = Parse("Hello world<done  />\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, StripDoneTagInMiddle) {
    auto elements = Parse("Hello <done /> world\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, StripMultipleDoneTags) {
    auto elements = Parse("<done />Hello<done/> world<done />\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseBoldAndCode) {
    auto elements = Parse("**bold** and `code` together\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseListItem) {
    auto elements = Parse("- First item\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseMultipleListItems) {
    auto first = parser_->parseChunk("- First item\n");
    EXPECT_EQ(first.size(), 0u);

    auto second = parser_->parseChunk("- Second item\n");
    EXPECT_EQ(second.size(), 1u);
}

TEST_F(MarkdownParserTest, ResetClearsState) {
    parser_->parseChunk("**partial bold");
    parser_->reset();

    auto elements = Parse("new text\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, FinishReturnsRemainingContent) {
    parser_->parseChunk("Unfinished paragraph");
    auto remaining = parser_->finish();
    EXPECT_EQ(remaining.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseEmptyString) {
    auto elements = parser_->parseChunk("");
    EXPECT_EQ(elements.size(), 0u);
}

TEST_F(MarkdownParserTest, ParseOnlyWhitespace) {
    auto elements = parser_->parseChunk("   \n\t  \n");
    EXPECT_EQ(elements.size(), 0u);
}

TEST_F(MarkdownParserTest, StreamFenceAcrossMultipleChunks) {
    auto part1 = parser_->parseChunk("```py\n");
    EXPECT_EQ(part1.size(), 0u);

    auto part2 = parser_->parseChunk("line1\n");
    EXPECT_EQ(part2.size(), 0u);

    auto part3 = parser_->parseChunk("line2\n");
    EXPECT_EQ(part3.size(), 0u);

    auto part4 = parser_->parseChunk("```\n");
    EXPECT_EQ(part4.size(), 1u);
}

TEST_F(MarkdownParserTest, StreamBoldAcrossLineBreak) {
    auto part1 = parser_->parseChunk("**start");
    EXPECT_EQ(part1.size(), 0u);

    auto part2 = parser_->parseChunk("bold**\n");
    EXPECT_EQ(part2.size(), 0u);

    auto finished = parser_->finish();
    EXPECT_EQ(finished.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseOnlyDoneTag) {
    auto elements = parser_->parseChunk("<done/>\n");
    EXPECT_EQ(elements.size(), 0u);
}

TEST_F(MarkdownParserTest, ParseBoldWithAsterisksInside) {
    auto elements = Parse("**has * asterisk**\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, ParseCodeWithBacktickInside) {
    auto elements = Parse("`has `` backtick`\n");
    EXPECT_EQ(elements.size(), 1u);
}

TEST_F(MarkdownParserTest, TextToFenceTransition) {
    auto text = parser_->parseChunk("Some text\n");
    EXPECT_EQ(text.size(), 0u);
    
    auto finished = parser_->finish();
    EXPECT_EQ(finished.size(), 1u);

    auto fence = parser_->parseChunk("```\ncode\n```\n");
    EXPECT_EQ(fence.size(), 1u);
}

TEST_F(MarkdownParserTest, FenceToTextTransition) {
    auto fence = parser_->parseChunk("```\ncode\n```\n");
    EXPECT_EQ(fence.size(), 1u);

    auto text = parser_->parseChunk("More text\n");
    EXPECT_EQ(text.size(), 0u);

    auto finished = parser_->finish();
    EXPECT_EQ(finished.size(), 1u);
}
