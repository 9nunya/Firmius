#include "utils/ToolSummaries.hpp"

#include <gtest/gtest.h>

using namespace firmius::shared;

TEST(ToolSummaries, SummarizesWorkLanguageTools) {
  EXPECT_EQ(SummarizeToolCall("plan_create",
                              R"({"title":"Implement Linux x86_64 backend"})",
                              ToolPhase::Called),
            "Create plan \"Implement Linux x86_64 backend\"");
  EXPECT_EQ(SummarizeToolCall("plan_update",
                              R"({"title":"Implement Linux x86_64 backend"})",
                              ToolPhase::Called),
            "Update plan \"Implement Linux x86_64 backend\"");
  EXPECT_EQ(SummarizeToolCall("plan_get", R"({"plan_id":"plan-1"})",
                              ToolPhase::Called),
            "Load plan");
  EXPECT_EQ(SummarizeToolCall("plan_list", "{}", ToolPhase::Called),
            "List plans");
  EXPECT_EQ(SummarizeToolCall("plan_set_active", R"({"plan_id":"plan-1"})",
                              ToolPhase::Called),
            "Set active plan");

  EXPECT_EQ(SummarizeToolCall("chunk_add",
                              R"({"title":"Inventory Xen semantics"})",
                              ToolPhase::Called),
            "Add chunk \"Inventory Xen semantics\"");
  EXPECT_EQ(SummarizeToolCall("chunk_get",
                              R"({"title":"Inventory Xen semantics"})",
                              ToolPhase::Called),
            "Load chunk \"Inventory Xen semantics\"");
  EXPECT_EQ(SummarizeToolCall("chunk_list", R"({"plan_id":"plan-1"})",
                              ToolPhase::Called),
            "List chunks");
  EXPECT_EQ(SummarizeToolCall("chunk_update",
                              R"({"title":"Architect Native Backend"})",
                              ToolPhase::Called),
            "Update chunk \"Architect Native Backend\"");
  EXPECT_EQ(SummarizeToolCall("chunk_ready_for_execution",
                              R"({"plan_id":"plan-1"})", ToolPhase::Called),
            "Find executable chunks");
  EXPECT_EQ(SummarizeToolCall("python_execute",
                              R"({"code":"print('hi')\n"})",
                              ToolPhase::Called),
            "Python \"print('hi')\"");
  EXPECT_EQ(SummarizeToolCall(
                "file_edit",
                R"({"path":"src/foo.rs","edits":[{"op":"insert_after","anchor":"12#f828","new_lines":["let x = 1;"]},{"op":"delete_range","start_anchor":"18#a1bc","end_anchor":"19#beef"}]})",
                ToolPhase::Called),
            "Edit src/foo.rs (2 ops)");
  EXPECT_EQ(SummarizeToolCall("file_edit",
                              R"({"path":"src/foo.rs","content":"fn main() {}\n"})",
                              ToolPhase::Called),
            "Overwrite src/foo.rs");
}
