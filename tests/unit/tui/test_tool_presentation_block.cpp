#include "components/ToolPresentationBlock.hpp"
#include "tools/ToolPresentation.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

namespace {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;
using firmius::tui::BuildToolPresentation;
using firmius::tui::ToolPresentationBlock;

std::string Render(const std::shared_ptr<ToolCallView> &view, int width = 120,
                   int height = 20) {
  auto component =
      ToolPresentationBlock(view, [view] { return BuildToolPresentation(*view); });
  auto element = component->Render();
  ftxui::Screen screen(width, height);
  ftxui::Render(screen, element);
  return screen.ToString();
}

TEST(ToolPresentationBlockTest, CollapsedGenericToolRender) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "custom_tool";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = "alpha\nbeta\ngamma\ndelta\nepsilon\nzeta\n";

  const std::string output = Render(view);
  EXPECT_NE(output.find("custom_tool"), std::string::npos);
  EXPECT_NE(output.find("show"), std::string::npos);
  EXPECT_EQ(output.find("zeta"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ExpandedGenericToolRender) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "custom_tool";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = true;
  view->result = "line 1\nline 2\nline 3\nline 4\nline 5\nline 6\n";

  const std::string output = Render(view, 120, 120);
  EXPECT_NE(output.find("hide"), std::string::npos);
  EXPECT_NE(output.find("custom_tool"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ErrorStateRender) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "custom_tool";
  view->phase = ToolPhase::Finished;
  view->success = false;
  view->result = "fatal: crash";

  const std::string output = Render(view);
  EXPECT_NE(output.find("failed"), std::string::npos);
  EXPECT_NE(output.find("crash"), std::string::npos);
}

TEST(ToolPresentationBlockTest, LifecycleBranchSelectionShowsExpectedTitles) {
  auto preparing = std::make_shared<ToolCallView>();
  preparing->name = "custom_tool";
  preparing->phase = ToolPhase::Preparing;

  auto running = std::make_shared<ToolCallView>();
  running->name = "custom_tool";
  running->phase = ToolPhase::Called;

  auto success = std::make_shared<ToolCallView>();
  success->name = "custom_tool";
  success->phase = ToolPhase::Finished;
  success->success = true;

  EXPECT_NE(Render(preparing).find("Preparing"), std::string::npos);
  EXPECT_NE(Render(running).find("custom_tool"), std::string::npos);
  EXPECT_NE(Render(success).find("custom_tool"), std::string::npos);
}

TEST(ToolPresentationBlockTest, GrepRenderShowsSearchRowsNotRawTailPreview) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "grep";
  view->args = R"({"pattern":"needle","path":"src"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = true;
  view->result = R"([
    {"path":"src/a.cpp","line_number":4,"line":"needle one"},
    {"path":"src/b.cpp","line_number":9,"line":"needle two"}
  ])";

  const std::string output = Render(view);
  EXPECT_NE(output.find("Matches"), std::string::npos);
  EXPECT_NE(output.find("src/a.cpp:4"), std::string::npos);
  EXPECT_NE(output.find("2"), std::string::npos);
  EXPECT_EQ(output.find("Result preview"), std::string::npos);
}

TEST(ToolPresentationBlockTest, GlobRenderShowsPathRowsWithTypeHints) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "glob";
  view->args = R"({"pattern":"src/**"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = true;
  view->result = R"([
    {"path":"src/core","type":"directory"},
    {"path":"src/core/main.cpp","type":"file"}
  ])";

  const std::string output = Render(view);
  EXPECT_NE(output.find("[dir] src/core"), std::string::npos);
  EXPECT_NE(output.find("[file] src/core/main.cpp"), std::string::npos);
  EXPECT_NE(output.find("Matches"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ProcessExecuteRenderUsesRichFactsNotGenericPreview) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"sleep 1"})";
  view->phase = ToolPhase::Called;
  view->show_result = false;
  view->result = "line 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\n";

  const std::string output = Render(view);
  EXPECT_NE(output.find("$ sleep 1"), std::string::npos);
  EXPECT_NE(output.find("running"), std::string::npos);
  EXPECT_NE(output.find("╰"), std::string::npos);
  EXPECT_EQ(output.find("State:"), std::string::npos);
  EXPECT_EQ(output.find("Result preview"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ProcessWaitRenderIncludesPatternAndProcess) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_wait";
  view->args = R"({"process_id":"proc-1","pattern":"READY"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->result = R"({"isRunning":false,"patternFound":true,"stdout":"READY\n"})";
  view->show_result = true;

  const std::string output = Render(view);
  EXPECT_NE(output.find("proc-1"), std::string::npos);
  EXPECT_NE(output.find("READY"), std::string::npos);
  EXPECT_NE(output.find("╰"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ProcessStatusRenderShowsRunningOrFinishedFacts) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_status";
  view->args = R"({"process_id":"proc-s"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->result = R"({"isRunning":false,"exitCode":0,"duration_ms":100})";

  const std::string output = Render(view);
  EXPECT_NE(output.find("status"), std::string::npos);
  EXPECT_NE(output.find("exit 0"), std::string::npos);
  EXPECT_NE(output.find("╰"), std::string::npos);
}

TEST(ToolPresentationBlockTest, PythonExecuteRenderUsesProcessFamilyShape) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "python_execute";
  view->args = R"({"code":"print('hello')\n"})";
  view->phase = ToolPhase::Finished;
  view->success = true;

  const std::string output = Render(view);
  EXPECT_NE(output.find("python"), std::string::npos);
  EXPECT_NE(output.find("$ python"), std::string::npos);
}

TEST(ToolPresentationBlockTest, SummonSubagentRenderUsesRichFactsAndActivity) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "summon_subagent";
  view->args = R"({"title":"Worker","task":"Implement login"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = true;
  view->subagent_tool_log = {
      {"Search \"**/*\" in .", ToolPhase::Finished, "", "", ""},
      {"Loaded @artifact:worker/report.md", ToolPhase::Finished, "", "", ""},
      {"Done", ToolPhase::Finished, "", "", ""},
  };
  view->result = R"({"agentId":"child-1","status":"completed","result":"done","fallback_used":true,"category":"scout","attempted_categories":["executor","scout"],"artifacts_created":[{"reference":"@artifact:worker/report.md"}],"artifacts_updated":[]})";

  const std::string output = Render(view);
  EXPECT_NE(output.find("delegate"), std::string::npos);
  EXPECT_NE(output.find("Fallback"), std::string::npos);
  EXPECT_NE(output.find("+1 artifact"), std::string::npos);
  EXPECT_NE(output.find("Search \"**/*\" in ."), std::string::npos);
  EXPECT_NE(output.find("Loaded @artifact:worker/report.md"), std::string::npos);
  EXPECT_NE(output.find("│ "), std::string::npos);
  EXPECT_EQ(output.find("Result preview"), std::string::npos);
}

TEST(ToolPresentationBlockTest, SubagentWaitRenderShowsOutcomeWithoutGenericFallback) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "subagent_wait";
  view->args = R"({"agent_id":"child-2"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = true;
  view->result = R"({"agentId":"child-2","status":"completed_no_summary","result":"","fallback_used":false,"attempted_categories":["worker"],"artifacts_created":[{"reference":"@artifact:worker/out.md"}],"artifacts_updated":[{"reference":"@artifact:worker/state.json"}]})";

  const std::string output = Render(view);
  EXPECT_NE(output.find("waiting"), std::string::npos);
  EXPECT_NE(output.find("+1 artifact"), std::string::npos);
  EXPECT_EQ(output.find("Result preview"), std::string::npos);
  EXPECT_EQ(output.find("│ "), std::string::npos);
  EXPECT_EQ(output.find("show"), std::string::npos);
}

TEST(ToolPresentationBlockTest, TierARichToolsRenderStructuredSections) {
  auto plan = std::make_shared<ToolCallView>();
  plan->name = "plan_get";
  plan->phase = ToolPhase::Finished;
  plan->success = true;
  plan->result = R"({"id":"plan-1","title":"Ship","status":"Active","objective":"release","context":"ctx","strategy":"strat","chunks":[{"planning_gate":true}]})";

  auto chunk = std::make_shared<ToolCallView>();
  chunk->name = "chunk_get";
  chunk->phase = ToolPhase::Finished;
  chunk->success = true;
  chunk->result = R"({"id":"c-1","title":"Chunk","status":"Ready","goal":"g","depends_on":["c-0"],"files_to_read":["a.cpp"],"files_to_touch":["b.cpp"],"cwd":"/repo","verification_condition":"tests","handoff_notes":"handoff"})";

  const std::string plan_output = Render(plan);
  const std::string chunk_output = Render(chunk);
  EXPECT_NE(plan_output.find("Plan summary"), std::string::npos);
  EXPECT_NE(chunk_output.find("Execution details"), std::string::npos);
  EXPECT_EQ(plan_output.find("Result preview"), std::string::npos);
  EXPECT_EQ(chunk_output.find("Result preview"), std::string::npos);
}

TEST(ToolPresentationBlockTest, TierBAndTierCRenderCompactButInformative) {
  auto read = std::make_shared<ToolCallView>();
  read->name = "file_read";
  read->args = R"({"path":"src/main.cpp"})";
  read->phase = ToolPhase::Finished;
  read->success = true;
  read->result = R"({"content":"a\nb\nc","line_start":1,"line_end":3,"lines_read":3})";

  auto list = std::make_shared<ToolCallView>();
  list->name = "list_directory";
  list->args = R"({"path":"src"})";
  list->phase = ToolPhase::Finished;
  list->success = true;
  list->result = R"([{"name":"core","is_directory":true},{"name":"main.cpp","is_directory":false}])";

  const std::string read_output = Render(read);
  const std::string list_output = Render(list);
  EXPECT_NE(read_output.find("Preview"), std::string::npos);
  EXPECT_NE(list_output.find("Entries"), std::string::npos);
  EXPECT_EQ(list_output.find("Result preview"), std::string::npos);
}

TEST(ToolPresentationBlockTest, WorkMutationsRenderAsOneLineSummaries) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "chunk_add";
  view->args =
      R"({"plan_id":"plan-1","title":"Chunk A","goal":"goal","tasks":[{"id":"task-1","title":"Task 1","goal":"Goal 1"}]})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->result = R"({"chunk_id":"c-1","status":"Ready"})";

  const std::string output = Render(view, 80, 8);
  EXPECT_NE(output.find("Chunk A"), std::string::npos);
  EXPECT_NE(output.find(""), std::string::npos);
  EXPECT_EQ(output.find("Goal:"), std::string::npos);
  EXPECT_EQ(output.find("Result preview"), std::string::npos);
}

TEST(ToolPresentationBlockTest, RemainingFamiliesUseCentralizedPresenters) {
  auto artifact = std::make_shared<ToolCallView>();
  artifact->name = "artifact_list";
  artifact->phase = ToolPhase::Finished;
  artifact->success = true;
  artifact->result = R"({"artifacts":[{"display":"lead/REPORT.md","reference":"@artifact:lead/REPORT.md","ambiguous_filename":false}]})";

  auto work = std::make_shared<ToolCallView>();
  work->name = "todo_write";
  work->args = R"({"patch":"1. [+] add\n"})";
  work->phase = ToolPhase::Finished;
  work->success = true;
  work->result = R"({"items":[{"id":1,"status":"pending","text":"add"}]})";

  auto web = std::make_shared<ToolCallView>();
  web->name = "web_fetch";
  web->args = R"({"url":"https://example.com"})";
  web->phase = ToolPhase::Finished;
  web->success = true;
  web->result = R"({"size":22,"content":"Title\nBody"})";

  const std::string artifact_output = Render(artifact);
  const std::string work_output = Render(work);
  const std::string web_output = Render(web);
  EXPECT_NE(artifact_output.find("Artifacts"), std::string::npos);
  EXPECT_NE(work_output.find("todo list updated"), std::string::npos);
  EXPECT_NE(work_output.find("+"), std::string::npos);
  EXPECT_NE(web_output.find("Excerpt"), std::string::npos);
  EXPECT_EQ(artifact_output.find("Result preview"), std::string::npos);
  EXPECT_EQ(work_output.find("Result preview"), std::string::npos);
  EXPECT_EQ(web_output.find("Result preview"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ExpandToggleRevealsAdditionalContentWhenEnabled) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "file_read";
  view->args = R"({"path":"src/main.cpp"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"content":"row01\nrow02\nrow03\nrow04\nrow05\nrow06\nrow07\nrow08\nrow09\nrow10\nrow11\nrow12\nrow13\nrow14_tail\n","line_start":1,"line_end":14,"lines_read":14})";

  const std::string collapsed = Render(view, 120, 28);
  EXPECT_NE(collapsed.find("show"), std::string::npos);
  EXPECT_EQ(collapsed.find("row14_tail"), std::string::npos);

  view->show_result = true;
  const std::string expanded = Render(view, 120, 120);
  EXPECT_NE(expanded.find("hide"), std::string::npos);
  EXPECT_NE(expanded.find("row14_tail"), std::string::npos);
}

TEST(ToolPresentationBlockTest, BodyFirstProcessCardShowsOutputWindowStyle) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"echo hello"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"stdout":"hello\nworld\n","exit_code":0,"duration_ms":12})";

  const std::string output = Render(view);
  EXPECT_NE(output.find("$ echo hello"), std::string::npos);
  EXPECT_NE(output.find("│ hello"), std::string::npos);
  EXPECT_NE(output.find("exit 0"), std::string::npos);
  EXPECT_NE(output.find("╰"), std::string::npos);
  EXPECT_NE(output.find("hello"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ProcessCollapsedCardKeepsMetadataSecondary) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"echo hello"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"stdout":"hello\nworld\n","exit_code":0,"duration_ms":12})";

  const std::string output = Render(view, 120, 28);
  EXPECT_NE(output.find("$ echo hello"), std::string::npos);
  EXPECT_EQ(output.find("State:"), std::string::npos);
  EXPECT_EQ(output.find("details"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ProcessExpandedCardShowExpandsOutputDepthFirst) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"echo hello"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"stdout":"line01\nline02\nline03\nline04\nline05\nline06\nline07\nline08\nline09\nline10\nline11\nline12\n","exit_code":0,"duration_ms":12})";

  const std::string collapsed = Render(view, 120, 28);
  EXPECT_NE(collapsed.find("show more"), std::string::npos);
  EXPECT_EQ(collapsed.find("line01"), std::string::npos);
  EXPECT_NE(collapsed.find("line12"), std::string::npos);

  view->show_result = true;
  const std::string expanded = Render(view, 120, 80);
  EXPECT_NE(expanded.find("line01"), std::string::npos);
  EXPECT_NE(expanded.find("line12"), std::string::npos);
  EXPECT_EQ(expanded.find("│ ... +"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ProcessExpandedToggleRendersOnlyOnce) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"echo hello"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"stdout":"line01\nline02\nline03\nline04\nline05\nline06\nline07\n","exit_code":0,"duration_ms":12})";

  const std::string output = Render(view, 140, 30);
  size_t count = 0;
  size_t pos = output.find("show more");
  while (pos != std::string::npos) {
    ++count;
    pos = output.find("show more", pos + 1);
  }
  EXPECT_EQ(count, 1u);
}

TEST(ToolPresentationBlockTest, ProcessShortOutputDoesNotRenderShowMoreButton) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"echo short"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"stdout":"line01\nline02\nline03\n","exit_code":0,"duration_ms":12})";

  const std::string output = Render(view, 120, 28);
  EXPECT_EQ(output.find("show more"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ProcessCommandAppearsExactlyOnce) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"echo only-once"})";
  view->phase = ToolPhase::Called;
  view->show_result = false;
  view->result = "a\nb\n";

  const std::string output = Render(view, 140, 30);
  const auto first = output.find("$ echo only-once");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(output.find("$ echo only-once", first + 1), std::string::npos);
}

TEST(ToolPresentationBlockTest, ProcessFooterIsEmbeddedInOutputWindow) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "process_execute";
  view->args = R"({"command":"echo footer"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"stdout":"x\ny\n","exit_code":0,"duration_ms":132})";

  const std::string output = Render(view, 140, 30);
  EXPECT_NE(output.find("╰"), std::string::npos);
  EXPECT_NE(output.find("exit 0"), std::string::npos);
  EXPECT_EQ(output.find("  •  exit 0"), std::string::npos);
}

TEST(ToolPresentationBlockTest, WebFetchRedirectedCaseShowsFollowUpInstruction) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "web_fetch";
  view->args = R"({"url":"https://example.com/large"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = true;
  view->result = R"({"size":120000,"redirected_to":"/tmp/large.md","content":"Content too large, saved to file.","instruction":"Use file_read or grep to inspect the saved content."})";

  const std::string output = Render(view, 140, 30);
  EXPECT_NE(output.find("Follow-up"), std::string::npos);
  EXPECT_NE(output.find("file_read or grep"), std::string::npos);
}

TEST(ToolPresentationBlockTest,
     WebFetchRedirectedInstructionIsVisibleWithoutExpansion) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "web_fetch";
  view->args = R"({"url":"https://example.com/large"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"size":120000,"redirected_to":"/tmp/large.md","content":"Content too large, saved to file.","instruction":"Use file_read or grep to inspect the saved content."})";

  const std::string output = Render(view, 140, 28);
  EXPECT_NE(output.find("Follow-up:"), std::string::npos);
  EXPECT_NE(output.find("file_read or grep"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ResultsListRowsRenderBeforeCountFooter) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "list_directory";
  view->args = R"({"path":"src"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"([{"name":"core","is_directory":true},{"name":"main.cpp","is_directory":false}])";

  const std::string output = Render(view, 120, 30);
  const auto row_pos = output.find("[dir] core");
  const auto footer_pos = output.find("2 entries");
  ASSERT_NE(row_pos, std::string::npos);
  ASSERT_NE(footer_pos, std::string::npos);
  EXPECT_LT(row_pos, footer_pos);
}

TEST(ToolPresentationBlockTest, SubagentInlineDefaultStaysConcise) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "summon_subagent";
  view->args = R"({"title":"Worker","task":"Implement login"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"agentId":"child-1","status":"completed","result":"done line 1\ndone line 2\ndone line 3\ndone line 4","fallback_used":false,"attempted_categories":["executor","scout"],"artifacts_created":[{"reference":"@artifact:worker/report.md"}],"artifacts_updated":[]})";

  const std::string output = Render(view, 140, 28);
  EXPECT_NE(output.find("delegate"), std::string::npos);
  EXPECT_NE(output.find("+1 artifact"), std::string::npos);
  EXPECT_EQ(output.find("@artifact:worker/report.md"), std::string::npos);
  EXPECT_EQ(output.find("details"), std::string::npos);
}

TEST(ToolPresentationBlockTest, ExpandedSubagentHistoryKeepsHideToggleVisible) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "summon_subagent";
  view->args = R"({"title":"Worker","task":"Implement login"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = true;
  view->subagent_tool_log = {
      {"Listed", ToolPhase::Finished, "", "", ""},
      {"Read README", ToolPhase::Finished, "", "", ""},
      {"Grep config", ToolPhase::Finished, "", "", ""},
      {"Loaded @artifact:worker/report.md", ToolPhase::Finished, "", "", ""},
      {"Thought", ToolPhase::Finished, "", "", ""},
      {"Wrote report", ToolPhase::Finished, "", "", ""},
      {"Done", ToolPhase::Finished, "", "", ""},
  };

  const std::string output = Render(view, 140, 60);
  EXPECT_NE(output.find("hide"), std::string::npos);
  EXPECT_NE(output.find("Loaded @artifact:worker/report.md"), std::string::npos);
}

TEST(ToolPresentationBlockTest, SummonSubagentInlineDoesNotFallbackToRawTaskBody) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "summon_subagent";
  view->args = R"({"title":"Scout","task":"Investigate every failing test path and report the full causal chain without shortening the task text"})";
  view->phase = ToolPhase::Called;
  view->show_result = false;

  const std::string output = Render(view, 140, 16);
  EXPECT_NE(output.find("delegate Scout"), std::string::npos);
  EXPECT_EQ(output.find("Investigate every failing test path"), std::string::npos);
}

TEST(ToolPresentationBlockTest, SummonSubagentFinishedDoesNotInjectSyntheticStateLine) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "summon_subagent";
  view->args = R"({"title":"Worker","task":"Read file"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"agentId":"child-1","status":"spawned"})";

  const std::string output = Render(view, 140, 16);
  EXPECT_NE(output.find("delegate Worker"), std::string::npos);
  EXPECT_EQ(output.find("state:"), std::string::npos);
}

TEST(ToolPresentationBlockTest, SuccessHeaderDoesNotRenderDuplicateCheckGlyph) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "custom_tool";
  view->phase = ToolPhase::Finished;
  view->success = true;

  const std::string output = Render(view, 120, 12);
  EXPECT_EQ(output.find("✓"), std::string::npos);
}

TEST(ToolPresentationBlockTest, TodoWriteInlineRendersCompactSummaryRows) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "todo_write";
  view->args = R"({"patch":"1. [*] update\n2. [+] add\n3. [-] remove\n"})";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = R"({"items":[{"id":1,"status":"in_progress","text":"update"},{"id":2,"status":"pending","text":"add"},{"id":3,"status":"cancelled","text":"remove"},{"id":4,"status":"pending","text":"extra"}]})";

  const std::string output = Render(view, 140, 30);
  EXPECT_NE(output.find("+1"), std::string::npos);
  EXPECT_NE(output.find("~"), std::string::npos);
  EXPECT_EQ(output.find("extra"), std::string::npos);
  EXPECT_EQ(output.find("Todo items"), std::string::npos);
}

TEST(ToolPresentationBlockTest, PlanListZeroIsUltraCompact) {
  auto view = std::make_shared<ToolCallView>();
  view->name = "plan_list";
  view->phase = ToolPhase::Finished;
  view->success = true;
  view->show_result = false;
  view->result = "[]";

  const std::string output = Render(view, 120, 20);
  EXPECT_NE(output.find("plans"), std::string::npos);
  EXPECT_EQ(output.find("Plan rows"), std::string::npos);
  EXPECT_EQ(output.find("│ "), std::string::npos);
}

} // namespace
