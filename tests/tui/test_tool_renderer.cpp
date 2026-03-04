#include <gtest/gtest.h>

#include "ToolRenderer.hpp"
#include <ftxui/screen/screen.hpp>
#include <ftxui/dom/elements.hpp>

using namespace firmius::tui;

class ToolRendererTest : public ::testing::Test {
protected:
    ftxui::Element RenderToElement(const ToolCallState& state) {
        return ToolRenderer::render(state);
    }

    std::string RenderToString(const ToolCallState& state, int width = 80, int height = 24) {
        auto element = RenderToElement(state);
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
        ftxui::Render(screen, element);
        return screen.ToString();
    }
};

TEST_F(ToolRendererTest, RenderAbortedTool) {
    ToolCallState state;
    state.name = "bash";
    state.isAborted = true;
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("Aborted"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderErrorTool) {
    ToolCallState state;
    state.name = "read_file";
    state.isError = true;
    state.result = "File not found";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("Error"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderBashTool) {
    ToolCallState state;
    state.name = "bash";
    state.args = R"({"command":"ls -la"})";
    state.result = "file1.txt\nfile2.txt";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("ls -la"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderBashWithError) {
    ToolCallState state;
    state.name = "bash";
    state.args = R"({"command":"invalid_cmd"})";
    state.result = "command not found";
    state.isError = true;
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("ERROR"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderReadFileTool) {
    ToolCallState state;
    state.name = "read_file";
    state.args = R"({"filePath":"/path/to/file.txt"})";
    state.result = "file contents here\nline 2";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("read_file"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderWriteFileTool) {
    ToolCallState state;
    state.name = "write_file";
    state.args = R"({"filePath":"/path/to/file.txt","content":"new content"})";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("write_file"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderGrepTool) {
    ToolCallState state;
    state.name = "grep";
    state.args = R"({"pattern":"test","path":"."})";
    state.result = "file1.txt:10\nfile2.txt:20";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("grep"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderGlobTool) {
    ToolCallState state;
    state.name = "glob";
    state.args = R"({"pattern":"*.cpp"})";
    state.result = "main.cpp\ntest.cpp\nutils.cpp";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("glob"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderSearchReplaceTool) {
    ToolCallState state;
    state.name = "search_replace";
    state.args = R"({"filePath":"/test.cpp","oldString":"foo","newString":"bar"})";
    state.result = "OK";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("search_replace"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderSummonSubagentTool) {
    ToolCallState state;
    state.name = "summon_subagent";
    state.args = R"({"persona":"coder","goal":"fix bug"})";
    state.result = "subagent-123";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("summon_subagent"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderSubagentWaitTool) {
    ToolCallState state;
    state.name = "subagent_wait";
    state.args = R"({"subagentId":"subagent-123"})";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("Waiting"), std::string::npos);
}

TEST_F(ToolRendererTest, RenderUnknownTool) {
    ToolCallState state;
    state.name = "unknown_tool";
    state.args = R"({"key":"value"})";
    state.result = "Some result";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("unknown_tool"), std::string::npos);
}

TEST_F(ToolRendererTest, DiffCalculationOldString) {
    ToolCallState state;
    state.name = "search_replace";
    state.args = R"({"filePath":"test.cpp","oldString":"old line 1\nold line 2","newString":"new line 1\nnew line 2"})";
    state.result = "OK";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("-old line 1"), std::string::npos);
}

TEST_F(ToolRendererTest, DiffCalculationNewString) {
    ToolCallState state;
    state.name = "search_replace";
    state.args = R"({"filePath":"test.cpp","oldString":"old content","newString":"new content"})";
    state.result = "OK";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("+new content"), std::string::npos);
}

TEST_F(ToolRendererTest, DiffTruncatesLongOutput) {
    ToolCallState state;
    state.name = "search_replace";
    std::string manyLines;
    for (int i = 0; i < 50; i++) {
        manyLines += "line " + std::to_string(i) + "\\n";
    }
    state.args = R"({"filePath":"test.cpp","oldString":")" + manyLines + R"(","newString":"replacement"})";
    state.result = "OK";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("…"), std::string::npos);
}

TEST_F(ToolRendererTest, WriteFileShowsAddedLines) {
    ToolCallState state;
    state.name = "write_file";
    state.args = R"({"filePath":"new.txt","content":"line 1\nline 2\nline 3"})";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("+line 1"), std::string::npos);
}

TEST_F(ToolRendererTest, WriteFileTruncatesLongContent) {
    ToolCallState state;
    state.name = "write_file";
    std::string manyLines;
    for (int i = 0; i < 50; i++) {
        manyLines += "content line " + std::to_string(i) + "\\n";
    }
    state.args = R"({"filePath":"new.txt","content":")" + manyLines + R"("})";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("…"), std::string::npos);
}

TEST_F(ToolRendererTest, ReadFileLimitsOutput) {
    ToolCallState state;
    state.name = "read_file";
    state.args = R"({"filePath":"/big/file.txt"})";
    std::string manyLines;
    for (int i = 0; i < 50; i++) {
        manyLines += "file content line " + std::to_string(i) + "\n";
    }
    state.result = manyLines;
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("…"), std::string::npos);
}

TEST_F(ToolRendererTest, GrepLimitsOutput) {
    ToolCallState state;
    state.name = "grep";
    state.args = R"({"pattern":"test"})";
    std::string manyMatches;
    for (int i = 0; i < 50; i++) {
        manyMatches += "file" + std::to_string(i) + ".txt:" + std::to_string(i) + "\n";
    }
    state.result = manyMatches;
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("…"), std::string::npos);
}

TEST_F(ToolRendererTest, InvalidJsonArgs) {
    ToolCallState state;
    state.name = "search_replace";
    state.args = "invalid json";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("Invalid JSON"), std::string::npos);
}

TEST_F(ToolRendererTest, BashWithoutJsonArgs) {
    ToolCallState state;
    state.name = "bash";
    state.args = "raw command string";
    state.result = "output";
    
    auto output = RenderToString(state);
    EXPECT_NE(output.find("raw command string"), std::string::npos);
}

TEST_F(ToolRendererTest, EmptyToolName) {
    ToolCallState state;
    state.name = "";
    state.args = "{}";
    
    auto output = RenderToString(state);
    EXPECT_FALSE(output.empty());
}
