#include <gtest/gtest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include "components/Markdown.hpp"
#include <string>

using namespace firmius::tui;
using namespace ftxui;

std::string RenderToString(Element element, int width = 80, int height = 20) {
    auto screen = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
    Render(screen, element);
    return screen.ToString();
}

TEST(MarkdownTest, BasicTableStructure) {
    std::string markdown = 
        "| Header 1 | Header 2 |\n"
        "|----------|----------|\n"
        "| Cell 1   | Cell 2   |";
    
    auto element = RenderMarkdown(markdown);
    std::string output = RenderToString(element);
    
    EXPECT_TRUE(output.find("Header 1") != std::string::npos);
    EXPECT_TRUE(output.find("Header 2") != std::string::npos);
    EXPECT_TRUE(output.find("Cell 1") != std::string::npos);
    EXPECT_TRUE(output.find("Cell 2") != std::string::npos);
    
    // We want box-drawing characters specifically, not just pipe chars
    bool has_proper_box_chars = (output.find("│") != std::string::npos) || 
                                (output.find("─") != std::string::npos);
    EXPECT_TRUE(has_proper_box_chars);
}

TEST(MarkdownTest, NestedBoldInCells) {
    std::string markdown = 
        "| Bold | Normal |\n"
        "|------|--------|\n"
        "| **Bold Content** | Plain |";
    
    auto element = RenderMarkdown(markdown);
    std::string output = RenderToString(element);
    
    EXPECT_TRUE(output.find("Bold Content") != std::string::npos);
    EXPECT_TRUE(output.find("Plain") != std::string::npos);
    // The current implementation renders raw markdown inside cells
    // because it doesn't call renderInline for table cells.
    EXPECT_EQ(output.find("**Bold Content**"), std::string::npos);
}

TEST(MarkdownTest, InlineCodeInCells) {
    std::string markdown = 
        "| Code | Description |\n"
        "|------|-------------|\n"
        "| `ls -la` | List files |";
    
    auto element = RenderMarkdown(markdown);
    std::string output = RenderToString(element);
    
    EXPECT_TRUE(output.find("ls -la") != std::string::npos);
    EXPECT_TRUE(output.find("List files") != std::string::npos);
}

TEST(MarkdownTest, PassthroughNonTableText) {
    std::string markdown = "Just some regular text before\n\n| T | B |\n|---|---|\n| 1 | 2 |\n\nAfter table text";
    
    auto element = RenderMarkdown(markdown);
    std::string output = RenderToString(element);
    
    EXPECT_TRUE(output.find("Just some regular text before") != std::string::npos);
    EXPECT_TRUE(output.find("After table text") != std::string::npos);
    EXPECT_TRUE(output.find("1") != std::string::npos);
}

TEST(MarkdownTest, AutoScalingLongText) {
    int width = 30;
    std::string long_text = "This-is-a-very-long-piece-of-text-that-should-definitely-wrap-within-the-table-cell.";
    std::string markdown = "| Long |\n|------|\n| " + long_text + " |";
    
    auto element = RenderMarkdown(markdown);
    std::string output = RenderToString(element, width);
    
    EXPECT_TRUE(output.find("This-is-a") != std::string::npos);
}
