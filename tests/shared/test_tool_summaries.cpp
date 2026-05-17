#include "utils/ToolSummaries.hpp"

#include <gtest/gtest.h>

using namespace firmius::shared;

TEST(ToolSummaries, SummarizesToolCalls) {
  EXPECT_EQ(SummarizeToolCall("Read", R"({"path":"src/foo.rs"})",
                              ToolPhase::Called),
            "Read src/foo.rs");
  EXPECT_EQ(SummarizeToolCall("Edit",
                              R"({"path":"src/foo.rs","edits":[{"op":"insert_after","anchor":"12","new_lines":["let x = 1;"]}]})",
                              ToolPhase::Called),
            "Edit src/foo.rs (1 ops)");
  EXPECT_EQ(SummarizeToolCall("Process",
                              R"({"action":"Execute","command":"echo hi"})",
                              ToolPhase::Called),
            "$ echo hi");
  EXPECT_EQ(SummarizeToolCall("Delegate",
                              R"({"action":"Spawn","title":"Auth finder"})",
                              ToolPhase::Called),
            "Delegate \"Auth finder\"");
  EXPECT_EQ(SummarizeToolCall("Web",
                              R"({"action":"Search","query":"firmius tool suite"})",
                              ToolPhase::Called),
            "Search the web for \"firmius tool suite\"");
  EXPECT_EQ(SummarizeToolCall("Python",
                              R"({"code":"print('hi')\n"})",
                              ToolPhase::Called),
            "Python \"print('hi')\"");
  EXPECT_EQ(SummarizeToolCall(
                "Edit",
                R"({"path":"src/foo.rs","edits":[{"op":"insert_after","anchor":"12","new_lines":["let x = 1;"]},{"op":"delete_range","start_anchor":"18","end_anchor":"19"}]})",
                ToolPhase::Called),
            "Edit src/foo.rs (2 ops)");
  EXPECT_EQ(SummarizeToolCall("Edit",
                              R"({"path":"src/foo.rs","content":"fn main() {}\n"})",
                              ToolPhase::Called),
            "Overwrite src/foo.rs");
}
