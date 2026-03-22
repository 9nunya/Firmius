#include "tools/ToolPresentation.hpp"

#include <algorithm>
#include <gtest/gtest.h>

namespace {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;
using firmius::tui::BuildToolPresentation;
using firmius::tui::NormalizedProcessState;
using firmius::tui::NormalizedSubagentState;
using firmius::tui::ToolPresentation;
using firmius::tui::ToolPresentationLayoutKind;
using firmius::tui::ToolPresentationLifecycle;

const std::string *FindFactValue(const ToolPresentation &presentation,
                                 const std::string &key) {
  for (const auto &fact : presentation.facts) {
    if (fact.key == key) {
      return &fact.value;
    }
  }
  return nullptr;
}

TEST(ToolPresentationTest, PreparingGenericToolPresentation) {
  ToolCallView view;
  view.name = "mystery_tool";
  view.phase = ToolPhase::Preparing;

  ToolPresentation presentation = BuildToolPresentation(view);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Preparing);
  EXPECT_NE(presentation.title.find("Preparing"), std::string::npos);
  EXPECT_EQ(presentation.subtitle, "mystery_tool");
  EXPECT_FALSE(presentation.expandable);
  EXPECT_FALSE(presentation.error_text.has_value());
}

TEST(ToolPresentationTest, FinishedSuccessGenericPresentation) {
  ToolCallView view;
  view.name = "mystery_tool";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = "line 1\nline 2\n";

  ToolPresentation presentation = BuildToolPresentation(view);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Success);
  EXPECT_TRUE(presentation.expandable);
  ASSERT_EQ(presentation.sections.size(), 1u);
  EXPECT_EQ(presentation.sections[0].title, "Result preview");
  ASSERT_EQ(presentation.sections[0].lines.size(), 2u);
  EXPECT_EQ(presentation.sections[0].lines[0], "line 1");
}

TEST(ToolPresentationTest, FinishedErrorGenericPresentation) {
  ToolCallView view;
  view.name = "mystery_tool";
  view.phase = ToolPhase::Finished;
  view.success = false;
  view.result = "tool failed: boom";

  ToolPresentation presentation = BuildToolPresentation(view);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Error);
  ASSERT_TRUE(presentation.error_text.has_value());
  EXPECT_NE(presentation.error_text.value().find("boom"), std::string::npos);
  EXPECT_NE(presentation.title.find("failed"), std::string::npos);
}

TEST(ToolPresentationTest, LongResultUsesTailPreviewAndNotice) {
  ToolCallView view;
  view.name = "mystery_tool";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = "line 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\n";

  ToolPresentation presentation = BuildToolPresentation(view);
  EXPECT_TRUE(presentation.expandable);
  EXPECT_FALSE(presentation.expanded);
  ASSERT_EQ(presentation.sections.size(), 1u);
  ASSERT_EQ(presentation.sections[0].lines.size(), 5u);
  EXPECT_EQ(presentation.sections[0].lines.front(), "line 4");
  EXPECT_EQ(presentation.sections[0].lines.back(), "line 8");
  ASSERT_EQ(presentation.notices.size(), 1u);
  EXPECT_NE(presentation.notices[0].text.find("last 5 of 8 lines"),
            std::string::npos);
}

TEST(ToolPresentationTest, FactsNoticesAndUnknownTitleContract) {
  ToolCallView view;
  view.name = "unknown_tool_family";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = "one\ntwo\nthree\nfour\nfive\nsix\n";

  ToolPresentation presentation = BuildToolPresentation(view);
  const std::string *tool_fact = FindFactValue(presentation, "Tool");
  ASSERT_NE(tool_fact, nullptr);
  EXPECT_EQ(*tool_fact, "unknown_tool_family");
  const std::string *lines_fact = FindFactValue(presentation, "Output lines");
  ASSERT_NE(lines_fact, nullptr);
  EXPECT_EQ(*lines_fact, "6");
  EXPECT_EQ(presentation.subtitle, "unknown_tool_family");
  EXPECT_NE(presentation.title.find("unknown_tool_family"), std::string::npos);
}

TEST(ToolPresentationTest, GrepPresentationIncludesMatchCountAndRows) {
  ToolCallView view;
  view.name = "grep";
  view.args = R"({"pattern":"NativeCompiler|emit","path":"src/compiler"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"([
    {"path":"src/compiler/a.cpp","line_number":12,"line":"emitNativeCompiler();"},
    {"path":"src/compiler/b.cpp","line_number":34,"line":"NativeCompiler state"}
  ])";

  ToolPresentation presentation = BuildToolPresentation(view);
  const std::string *matches = FindFactValue(presentation, "Matches");
  ASSERT_NE(matches, nullptr);
  EXPECT_EQ(*matches, "2");
  ASSERT_EQ(presentation.sections.size(), 1u);
  EXPECT_EQ(presentation.sections[0].title, "Matches");
  ASSERT_EQ(presentation.sections[0].lines.size(), 2u);
  EXPECT_NE(presentation.sections[0].lines[0].find("src/compiler/a.cpp:12"),
            std::string::npos);
}

TEST(ToolPresentationTest, GlobPresentationIncludesPathOrientedRows) {
  ToolCallView view;
  view.name = "glob";
  view.args = R"({"pattern":"src/**"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"([
    {"path":"src/compiler","type":"directory"},
    {"path":"src/compiler/a.cpp","type":"file"}
  ])";

  ToolPresentation presentation = BuildToolPresentation(view);
  const std::string *matches = FindFactValue(presentation, "Matches");
  ASSERT_NE(matches, nullptr);
  EXPECT_EQ(*matches, "2");
  ASSERT_EQ(presentation.sections.size(), 1u);
  ASSERT_EQ(presentation.sections[0].lines.size(), 2u);
  EXPECT_NE(presentation.sections[0].lines[0].find("[dir] src/compiler"),
            std::string::npos);
  EXPECT_NE(presentation.sections[0].lines[1].find("[file] src/compiler/a.cpp"),
            std::string::npos);
}

TEST(ToolPresentationTest, ProcessExecuteRunningPresentation) {
  ToolCallView view;
  view.name = "process_execute";
  view.args = R"({"command":"sleep 1"})";
  view.phase = ToolPhase::Called;

  NormalizedProcessState state;
  state.process_id = "proc-1";
  state.command = "sleep 1";
  state.running = true;
  state.is_background = false;

  ToolPresentation presentation = BuildToolPresentation(view, &state);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Running);
  EXPECT_TRUE(presentation.title.empty());
  ASSERT_FALSE(presentation.body_lines.empty());
  EXPECT_NE(presentation.body_lines.front().find("$ sleep 1"), std::string::npos);
  const std::string *proc = FindFactValue(presentation, "Process");
  ASSERT_NE(proc, nullptr);
  EXPECT_EQ(*proc, "proc-1");
}

TEST(ToolPresentationTest, ProcessExecuteTimeoutBackgroundPresentation) {
  ToolCallView view;
  view.name = "process_execute";
  view.args = R"({"command":"sleep 30"})";
  view.phase = ToolPhase::BackgroundRunning;
  view.success = true;
  view.result = R"({"finish_reason":"Timeout","process_id":"proc-2"})";

  NormalizedProcessState state;
  state.process_id = "proc-2";
  state.command = "sleep 30";
  state.running = true;
  state.is_background = true;

  ToolPresentation presentation = BuildToolPresentation(view, &state);
  ASSERT_TRUE(presentation.status_footer.has_value());
  EXPECT_NE(presentation.status_footer.value().find("background"), std::string::npos);
  ASSERT_FALSE(presentation.notices.empty());
  EXPECT_NE(presentation.notices.front().text.find("Timed out"), std::string::npos);
}

TEST(ToolPresentationTest, ProcessExecuteFinishedSuccessPresentation) {
  ToolCallView view;
  view.name = "process_execute";
  view.args = R"({"command":"echo ok"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"({"exit_code":0,"duration_ms":45})";

  NormalizedProcessState state;
  state.process_id = "proc-ok";
  state.command = "echo ok";
  state.finished = true;
  state.exit_code_known = true;
  state.exit_code = 0;
  state.duration_ms = 45.0;

  ToolPresentation presentation = BuildToolPresentation(view, &state);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Success);
  ASSERT_TRUE(presentation.status_footer.has_value());
  EXPECT_NE(presentation.status_footer.value().find("exit 0"), std::string::npos);
}

TEST(ToolPresentationTest, ProcessExecuteFinishedFailurePresentation) {
  ToolCallView view;
  view.name = "process_execute";
  view.args = R"({"command":"false"})";
  view.phase = ToolPhase::Finished;
  view.success = false;
  view.result = "exit 1";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Error);
  ASSERT_TRUE(presentation.error_text.has_value());
  EXPECT_NE(presentation.error_text.value().find("exit 1"), std::string::npos);
}

TEST(ToolPresentationTest, ProcessSpawnBackgroundPresentation) {
  ToolCallView view;
  view.name = "process_spawn";
  view.args = R"({"command":"tail -f app.log"})";
  view.phase = ToolPhase::BackgroundRunning;
  view.success = true;

  NormalizedProcessState state;
  state.process_id = "proc-spawn";
  state.command = "tail -f app.log";
  state.running = true;
  state.is_background = true;

  ToolPresentation presentation = BuildToolPresentation(view, &state);
  ASSERT_TRUE(presentation.status_footer.has_value());
  EXPECT_NE(presentation.status_footer.value().find("background"), std::string::npos);
}

TEST(ToolPresentationTest, ProcessWaitShowsProcessIdAndPattern) {
  ToolCallView view;
  view.name = "process_wait";
  view.args = R"({"process_id":"proc-3","pattern":"READY"})";
  view.phase = ToolPhase::Called;

  NormalizedProcessState state;
  state.process_id = "proc-3";
  state.command = "python server.py";
  state.waiting = true;

  ToolPresentation presentation = BuildToolPresentation(view, &state);
  EXPECT_NE(presentation.title.find("proc-3"), std::string::npos);
  const std::string *pattern = FindFactValue(presentation, "Pattern");
  ASSERT_NE(pattern, nullptr);
  EXPECT_EQ(*pattern, "READY");
}

TEST(ToolPresentationTest, ProcessInputCompactPreviewPresentation) {
  ToolCallView view;
  view.name = "process_input";
  view.args = R"({"process_id":"proc-4","input":"status\n"})";
  view.phase = ToolPhase::Finished;
  view.success = true;

  ToolPresentation presentation = BuildToolPresentation(view, nullptr);
  EXPECT_EQ(presentation.layout, ToolPresentationLayoutKind::BodyFirstStream);
  const std::string *input = FindFactValue(presentation, "Input");
  ASSERT_NE(input, nullptr);
  EXPECT_NE(input->find("status"), std::string::npos);
  ASSERT_FALSE(presentation.body_lines.empty());
  EXPECT_NE(presentation.body_lines.back().find("status"), std::string::npos);
}

TEST(ToolPresentationTest, ProcessStatusFinishedPresentation) {
  ToolCallView view;
  view.name = "process_status";
  view.args = R"({"process_id":"proc-5"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"({"isRunning":false,"exitCode":2,"duration_ms":3210})";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr);
  ASSERT_TRUE(presentation.status_footer.has_value());
  EXPECT_NE(presentation.status_footer.value().find("exit 2"), std::string::npos);
}

TEST(ToolPresentationTest, PythonExecuteUsesProcessFamilyPresentation) {
  ToolCallView view;
  view.name = "python_execute";
  view.args = R"({"code":"print('hi')\n"})";
  view.phase = ToolPhase::Finished;
  view.success = true;

  ToolPresentation presentation = BuildToolPresentation(view, nullptr);
  EXPECT_TRUE(presentation.title.empty());
  EXPECT_TRUE(presentation.subtitle.empty());
  ASSERT_FALSE(presentation.body_lines.empty());
  EXPECT_NE(presentation.body_lines.front().find("$ python"), std::string::npos);
}

TEST(ToolPresentationTest, SummonSubagentRunningPresentation) {
  ToolCallView view;
  view.name = "summon_subagent";
  view.args = R"({"title":"Worker","task":"Implement login"})";
  view.phase = ToolPhase::Called;

  NormalizedSubagentState state;
  state.parent_tool_call_id = "summon-1";
  state.child_agent_id = "child-1";
  state.child_title = "Worker";
  state.running = true;
  state.wait_state = "running";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr, &state);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Running);
  EXPECT_NE(presentation.title.find("delegate"), std::string::npos);
  EXPECT_NE(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "Worker"),
            presentation.footer_badges.end());
}

TEST(ToolPresentationTest, SummonSubagentRetryFallbackPresentation) {
  ToolCallView view;
  view.name = "summon_subagent";
  view.args = R"({"task":"Investigate flakes"})";
  view.phase = ToolPhase::Called;

  NormalizedSubagentState state;
  state.child_title = "Scout";
  state.running = true;
  state.retrying = true;
  state.wait_state = "retrying";
  state.fallback_used = true;
  state.route_category = "scout";
  state.attempted_categories = {"executor", "scout"};

  ToolPresentation presentation = BuildToolPresentation(view, nullptr, &state);
  const std::string *route = FindFactValue(presentation, "Route");
  ASSERT_NE(route, nullptr);
  EXPECT_EQ(*route, "scout");
  ASSERT_FALSE(presentation.notices.empty());
  EXPECT_NE(presentation.notices.front().text.find("Fallback"), std::string::npos);
}

TEST(ToolPresentationTest, SummonSubagentCompletedNoSummaryCancelledAndFailedPresentation) {
  ToolCallView base;
  base.name = "summon_subagent";
  base.args = R"({"title":"Worker","task":"Task"})";
  base.phase = ToolPhase::Finished;
  base.success = true;

  NormalizedSubagentState no_summary;
  no_summary.wait_state = "completed_no_summary";
  no_summary.outcome = firmius::tui::SubagentOutcomeKind::NoSummary;
  ToolPresentation p_no_summary = BuildToolPresentation(base, nullptr, &no_summary);
  EXPECT_FALSE(p_no_summary.notices.empty());

  NormalizedSubagentState cancelled;
  cancelled.wait_state = "cancelled";
  cancelled.outcome = firmius::tui::SubagentOutcomeKind::Cancelled;
  ToolPresentation p_cancelled = BuildToolPresentation(base, nullptr, &cancelled);
  bool has_cancel_notice = false;
  for (const auto &n : p_cancelled.notices) {
    if (n.text.find("cancelled") != std::string::npos) {
      has_cancel_notice = true;
    }
  }
  EXPECT_TRUE(has_cancel_notice);

  ToolCallView failed = base;
  failed.success = false;
  failed.phase = ToolPhase::Error;
  failed.result = "boom";
  NormalizedSubagentState failed_state;
  failed_state.wait_state = "failed";
  failed_state.outcome = firmius::tui::SubagentOutcomeKind::Failed;
  failed_state.error_text = "boom";
  ToolPresentation p_failed = BuildToolPresentation(failed, nullptr, &failed_state);
  EXPECT_EQ(p_failed.lifecycle, ToolPresentationLifecycle::Error);
  ASSERT_TRUE(p_failed.error_text.has_value());
  EXPECT_NE(p_failed.error_text.value().find("boom"), std::string::npos);
}

TEST(ToolPresentationTest, SubagentWaitPresentationShowsOutcomeAndArtifacts) {
  ToolCallView view;
  view.name = "subagent_wait";
  view.args = R"({"agent_id":"child-1"})";
  view.phase = ToolPhase::Finished;
  view.success = true;

  NormalizedSubagentState state;
  state.child_agent_id = "child-1";
  state.child_title = "Worker";
  state.wait_state = "completed";
  state.outcome = firmius::tui::SubagentOutcomeKind::Response;
  state.final_summary = "Done";
  state.artifacts_created = {"@artifact:worker/report.md"};
  state.artifacts_updated = {"@artifact:worker/index.json"};

  ToolPresentation presentation = BuildToolPresentation(view, nullptr, &state);
  EXPECT_NE(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "+1 artifact(s)"),
            presentation.footer_badges.end());
  EXPECT_NE(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "~1 artifact(s)"),
            presentation.footer_badges.end());
  EXPECT_LE(presentation.sections.size(), 1u);
}

TEST(ToolPresentationTest, SummonUsesFriendlyNameWhenTitleAbsent) {
  ToolCallView view;
  view.name = "summon_subagent";
  view.args = R"({"task":"Investigate"})";
  view.phase = ToolPhase::Called;

  NormalizedSubagentState state;
  state.child_agent_id = "child-7";
  state.child_friendly_name = "scout-worker";
  state.running = true;
  state.wait_state = "running";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr, &state);
  EXPECT_NE(presentation.title.find("scout-worker"), std::string::npos);
  EXPECT_NE(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "scout-worker"),
            presentation.footer_badges.end());
}

TEST(ToolPresentationTest, WaitUsesFriendlyNameWhenTitleAbsent) {
  ToolCallView view;
  view.name = "subagent_wait";
  view.args = R"({"agent_id":"child-8"})";
  view.phase = ToolPhase::Called;

  NormalizedSubagentState state;
  state.child_agent_id = "child-8";
  state.child_friendly_name = "worker-eight";
  state.waiting = true;
  state.wait_state = "waiting";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr, &state);
  EXPECT_NE(presentation.title.find("worker-eight"), std::string::npos);
  EXPECT_NE(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "worker-eight"),
            presentation.footer_badges.end());
}

TEST(ToolPresentationTest, FallsBackToAgentIdWhenNoTitleOrFriendlyName) {
  ToolCallView view;
  view.name = "subagent_wait";
  view.args = R"({"agent_id":"child-9"})";
  view.phase = ToolPhase::Called;

  NormalizedSubagentState state;
  state.child_agent_id = "child-9";
  state.waiting = true;
  state.wait_state = "waiting";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr, &state);
  EXPECT_NE(presentation.title.find("child-9"), std::string::npos);
  EXPECT_NE(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "child-9"),
            presentation.footer_badges.end());
}

TEST(ToolPresentationTest, ArtifactWriteSuccessPresentationHasReferenceAndPreview) {
  ToolCallView view;
  view.name = "artifact_write";
  view.args = R"({"name":"REPORT.md","kind":"report","content":"line1\nline2\nline3"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result =
      R"({"status":"created","created":true,"updated":false,"reference":"@artifact:lead/REPORT.md","artifact":{"filename":"REPORT.md","owner_friendly_name":"lead","kind":"report","description":"Weekly report"}})";

  ToolPresentation presentation = BuildToolPresentation(view);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Success);
  const std::string *reference = FindFactValue(presentation, "Reference");
  ASSERT_NE(reference, nullptr);
  EXPECT_EQ(*reference, "@artifact:lead/REPORT.md");
  EXPECT_FALSE(presentation.body_lines.empty());
}

TEST(ToolPresentationTest, ArtifactReadPreparingAndErrorStatesAreExplicit) {
  ToolCallView preparing;
  preparing.name = "artifact_read";
  preparing.args = R"({"reference":"@artifact:lead/REPORT.md"})";
  preparing.phase = ToolPhase::Preparing;

  ToolPresentation p = BuildToolPresentation(preparing);
  EXPECT_EQ(p.lifecycle, ToolPresentationLifecycle::Preparing);
  EXPECT_NE(p.title.find("prepare"), std::string::npos);

  ToolCallView failed = preparing;
  failed.phase = ToolPhase::Error;
  failed.success = false;
  failed.result = "Artifact not found";
  ToolPresentation e = BuildToolPresentation(failed);
  EXPECT_EQ(e.lifecycle, ToolPresentationLifecycle::Error);
  ASSERT_TRUE(e.error_text.has_value());
  EXPECT_NE(e.error_text.value().find("not found"), std::string::npos);
}

TEST(ToolPresentationTest, ArtifactListShowsRowsAndCount) {
  ToolCallView view;
  view.name = "artifact_list";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"({"artifacts":[
    {"display":"lead/REPORT.md","reference":"@artifact:lead/REPORT.md","ambiguous_filename":true},
    {"display":"worker/NOTES.md","reference":"@artifact:worker/NOTES.md","ambiguous_filename":false}
  ]})";

  ToolPresentation presentation = BuildToolPresentation(view);
  const std::string *count = FindFactValue(presentation, "Count");
  ASSERT_NE(count, nullptr);
  EXPECT_EQ(*count, "2");
  ASSERT_FALSE(presentation.sections.empty());
  EXPECT_NE(presentation.sections.front().lines.front().find("ambiguous"),
            std::string::npos);
}

TEST(ToolPresentationTest, PlanCreateAndPlanUpdateUseRichChangedFields) {
  ToolCallView create;
  create.name = "plan_create";
  create.args = R"({"title":"Refactor","objective":"cleanup"})";
  create.phase = ToolPhase::Finished;
  create.success = true;
  create.result = R"({"plan_id":"plan-1","status":"Active","active":true})";
  ToolPresentation c = BuildToolPresentation(create);
  const std::string *plan = FindFactValue(c, "Plan ID");
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(*plan, "plan-1");

  ToolCallView update;
  update.name = "plan_update";
  update.args = R"({"plan_id":"plan-1","title":"Refactor v2","objective":"cleaner","status":"Paused"})";
  update.phase = ToolPhase::Finished;
  update.success = true;
  update.result = R"({"plan_id":"plan-1","status":"Paused"})";
  ToolPresentation u = BuildToolPresentation(update);
  const std::string *changed = FindFactValue(u, "Changed fields");
  ASSERT_NE(changed, nullptr);
  EXPECT_EQ(*changed, "3");
  ASSERT_FALSE(u.sections.empty());
  EXPECT_EQ(u.sections.front().title, "Updated fields");
}

TEST(ToolPresentationTest, PlanGetAndPlanListShowStructuredRows) {
  ToolCallView get;
  get.name = "plan_get";
  get.args = R"({"plan_id":"plan-1"})";
  get.phase = ToolPhase::Finished;
  get.success = true;
  get.result = R"({"id":"plan-1","title":"Ship","status":"Active","objective":"release","context":"ctx","strategy":"strat","chunks":[{"planning_gate":true},{"planning_gate":false}]})";
  ToolPresentation g = BuildToolPresentation(get);
  const std::string *chunks = FindFactValue(g, "Chunk count");
  ASSERT_NE(chunks, nullptr);
  EXPECT_EQ(*chunks, "2");
  ASSERT_FALSE(g.sections.empty());
  EXPECT_EQ(g.sections.front().title, "Plan summary");

  ToolCallView list;
  list.name = "plan_list";
  list.phase = ToolPhase::Finished;
  list.success = true;
  list.result = R"([
    {"plan_id":"plan-1","title":"Ship","status":"Active","is_active":true},
    {"plan_id":"plan-2","title":"Polish","status":"Draft","is_active":false}
  ])";
  ToolPresentation l = BuildToolPresentation(list);
  EXPECT_EQ(l.layout, ToolPresentationLayoutKind::CompactFactCard);
  EXPECT_NE(l.compact_summary.find("Ship"), std::string::npos);
}

TEST(ToolPresentationTest, ChunkAddGetListReadyUpdateHaveExplicitShapes) {
  ToolCallView add;
  add.name = "chunk_add";
  add.args = R"({"plan_id":"plan-1","title":"Chunk A","goal":"goal","depends_on":["c-0"],"planning_gate":true})";
  add.phase = ToolPhase::Finished;
  add.success = true;
  add.result = R"({"chunk_id":"c-1","status":"Blocked"})";
  ToolPresentation a = BuildToolPresentation(add);
  const std::string *chunk_id = FindFactValue(a, "Chunk ID");
  ASSERT_NE(chunk_id, nullptr);
  EXPECT_EQ(*chunk_id, "c-1");

  ToolCallView get;
  get.name = "chunk_get";
  get.args = R"({"plan_id":"plan-1","chunk_id":"c-1"})";
  get.phase = ToolPhase::Finished;
  get.success = true;
  get.result = R"({"id":"c-1","title":"Chunk A","status":"Ready","goal":"g","depends_on":["c-0"],"files_to_read":["a.cpp"],"files_to_touch":["b.cpp"],"cwd":"/repo","verification_condition":"tests pass","handoff_notes":"handoff"})";
  ToolPresentation g = BuildToolPresentation(get);
  ASSERT_FALSE(g.sections.empty());
  EXPECT_EQ(g.sections.front().title, "Execution details");

  ToolCallView list;
  list.name = "chunk_list";
  list.args = R"({"plan_id":"plan-1"})";
  list.phase = ToolPhase::Finished;
  list.success = true;
  list.result = R"([
    {"chunk_id":"c-1","title":"A","status":"Ready","depends_on":[]},
    {"chunk_id":"c-2","title":"B","status":"Blocked","depends_on":["c-1"]}
  ])";
  ToolPresentation cl = BuildToolPresentation(list);
  EXPECT_GE(cl.sections.size(), 2u);

  ToolCallView ready;
  ready.name = "chunk_ready_for_execution";
  ready.args = R"({"plan_id":"plan-1"})";
  ready.phase = ToolPhase::Finished;
  ready.success = true;
  ready.result = R"([])";
  ToolPresentation r = BuildToolPresentation(ready);
  ASSERT_FALSE(r.notices.empty());
  EXPECT_NE(r.notices.front().text.find("No ready chunks"), std::string::npos);

  ToolCallView update;
  update.name = "chunk_update";
  update.args = R"({"plan_id":"plan-1","chunk_id":"c-1","status":"InProgress","files_to_touch":["x.cpp"],"verification_condition":"ok"})";
  update.phase = ToolPhase::Finished;
  update.success = true;
  update.result = R"({"chunk_id":"c-1","status":"InProgress"})";
  ToolPresentation u = BuildToolPresentation(update);
  const std::string *changed = FindFactValue(u, "Changed fields");
  ASSERT_NE(changed, nullptr);
  EXPECT_EQ(*changed, "3");
}

TEST(ToolPresentationTest, TodoWriteShowsPatchCountsAndItems) {
  ToolCallView view;
  view.name = "todo_write";
  view.args = R"({"patch":"1. [+] add thing\n2. [x] done thing\n3. [-] remove thing\n"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"({"items":[
      {"id":1,"status":"pending","text":"add thing"},
      {"id":2,"status":"done","text":"done thing"}
    ]})";

  ToolPresentation p = BuildToolPresentation(view);
  EXPECT_NE(std::find(p.footer_badges.begin(), p.footer_badges.end(), "+1"),
            p.footer_badges.end());
  EXPECT_NE(std::find(p.footer_badges.begin(), p.footer_badges.end(), "done 1"),
            p.footer_badges.end());
  EXPECT_NE(std::find(p.footer_badges.begin(), p.footer_badges.end(), "cancelled 1"),
            p.footer_badges.end());
  EXPECT_FALSE(p.body_lines.empty());
  EXPECT_TRUE(p.sections.empty());
}

TEST(ToolPresentationTest, ArtifactReadUsesResolvedReferenceInTitle) {
  ToolCallView view;
  view.name = "artifact_read";
  view.args = R"({"reference":"@artifact:worker/out.md"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"({"reference":"@artifact:worker/out.md","artifact":{"filename":"out.md","owner_friendly_name":"worker"}})";

  ToolPresentation p = BuildToolPresentation(view);
  EXPECT_EQ(p.title, "loaded @artifact:worker/out.md");
  EXPECT_NE(std::find(p.footer_badges.begin(), p.footer_badges.end(), "worker"),
            p.footer_badges.end());
}

TEST(ToolPresentationTest, TodoWriteFallsBackToPatchExtractionFromJsonishArgs) {
  ToolCallView view;
  view.name = "todo_write";
  view.args =
      "{\n"
      "  \"patch\": \"1. [*] update\\n2. [+] add\\n3. [-] remove\\n\"\n"
      "}";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"({"items":[]})";

  ToolPresentation p = BuildToolPresentation(view);
  EXPECT_NE(std::find(p.footer_badges.begin(), p.footer_badges.end(), "+1"),
            p.footer_badges.end());
  EXPECT_NE(std::find(p.body_lines.begin(), p.body_lines.end(), "~ update"),
            p.body_lines.end());
  EXPECT_NE(std::find(p.body_lines.begin(), p.body_lines.end(), "+ add"),
            p.body_lines.end());
  EXPECT_NE(std::find(p.body_lines.begin(), p.body_lines.end(), "- remove"),
            p.body_lines.end());
}

TEST(ToolPresentationTest, TodoWritePrefersDeltaAgainstPreviousSnapshot) {
  ToolCallView view;
  view.name = "todo_write";
  view.args = R"({"patch":"1. [ ] unchanged\n2. [*] updated\n3. [ ] untouched\n"})";
  view.previous_result = R"({"items":[
      {"id":1,"status":"pending","text":"unchanged"},
      {"id":2,"status":"pending","text":"updated"},
      {"id":3,"status":"pending","text":"untouched"}
    ]})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"({"items":[
      {"id":1,"status":"pending","text":"unchanged"},
      {"id":2,"status":"in_progress","text":"updated"},
      {"id":3,"status":"pending","text":"untouched"}
    ]})";

  ToolPresentation p = BuildToolPresentation(view);
  ASSERT_EQ(p.body_lines.size(), 1u);
  EXPECT_EQ(p.body_lines.front(), "~ updated");
}

TEST(ToolPresentationTest, FileReadFileEditAndListDirectoryHaveExplicitStates) {
  ToolCallView read;
  read.name = "file_read";
  read.args = R"({"path":"src/main.cpp","start_line":1,"end_line":12})";
  read.phase = ToolPhase::Finished;
  read.success = true;
  read.result = R"({"content":"a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\n","line_start":1,"line_end":13,"lines_read":13})";
  ToolPresentation r = BuildToolPresentation(read);
  EXPECT_EQ(r.lifecycle, ToolPresentationLifecycle::Success);
  EXPECT_TRUE(r.expandable);
  ASSERT_FALSE(r.sections.empty());
  EXPECT_EQ(r.sections.front().title, "Preview");

  ToolCallView edit;
  edit.name = "file_edit";
  edit.args = R"({"path":"src/main.cpp","edits":[{"op":"replace_range"}]})";
  edit.phase = ToolPhase::Called;
  ToolPresentation e_running = BuildToolPresentation(edit);
  EXPECT_EQ(e_running.lifecycle, ToolPresentationLifecycle::Running);
  EXPECT_NE(e_running.title.find("editing"), std::string::npos);

  edit.phase = ToolPhase::Finished;
  edit.success = true;
  edit.result = R"({"operations":[{"op":"replace_range","description":"replace line","old_lines":["a"],"new_lines":["b"]}]})";
  ToolPresentation e = BuildToolPresentation(edit);
  const std::string *ops = FindFactValue(e, "Operations");
  const std::string *adds = FindFactValue(e, "Added lines");
  const std::string *removes = FindFactValue(e, "Removed lines");
  ASSERT_NE(ops, nullptr);
  ASSERT_NE(adds, nullptr);
  ASSERT_NE(removes, nullptr);
  EXPECT_EQ(*ops, "1");
  EXPECT_EQ(*adds, "1");
  EXPECT_EQ(*removes, "1");

  ToolCallView ls;
  ls.name = "list_directory";
  ls.args = R"({"path":"src"})";
  ls.phase = ToolPhase::Finished;
  ls.success = true;
  ls.result = R"([
    {"name":"core","is_directory":true},
    {"name":"main.cpp","is_directory":false}
  ])";
  ToolPresentation l = BuildToolPresentation(ls);
  const std::string *entries = FindFactValue(l, "Entries");
  ASSERT_NE(entries, nullptr);
  EXPECT_EQ(*entries, "2");
  ASSERT_FALSE(l.sections.empty());
}

TEST(ToolPresentationTest, WebFetchAndTerminateSubagentPresentersAreExplicit) {
  ToolCallView fetch;
  fetch.name = "web_fetch";
  fetch.args = R"({"url":"https://example.com/page"})";
  fetch.phase = ToolPhase::Finished;
  fetch.success = true;
  fetch.result = R"({"size":123,"content":"Title\nBody line 1\nBody line 2","redirected_to":"/tmp/file.md"})";
  ToolPresentation wf = BuildToolPresentation(fetch);
  const std::string *size = FindFactValue(wf, "Size");
  ASSERT_NE(size, nullptr);
  EXPECT_NE(size->find("123"), std::string::npos);
  ASSERT_FALSE(wf.sections.empty());
  ASSERT_FALSE(wf.notices.empty());

  ToolCallView term;
  term.name = "terminate_subagent";
  term.args = R"({"agent_id":"child-1"})";
  term.phase = ToolPhase::Finished;
  term.success = true;
  term.result = R"({"agent_id":"child-1","status":"terminated"})";
  ToolPresentation t = BuildToolPresentation(term);
  const std::string *subagent = FindFactValue(t, "Subagent");
  const std::string *status = FindFactValue(t, "Status");
  ASSERT_NE(subagent, nullptr);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(*subagent, "child-1");
  EXPECT_EQ(*status, "terminated");
}

TEST(ToolPresentationTest, LongProcessCommandIsNotDestructivelyTruncated) {
  ToolCallView view;
  view.name = "process_execute";
  view.args = R"({"command":"python script.py --with very long arguments and full command visibility"})";
  view.phase = ToolPhase::Called;

  ToolPresentation p = BuildToolPresentation(view, nullptr);
  ASSERT_FALSE(p.body_lines.empty());
  EXPECT_NE(p.body_lines.front().find("$ python script.py --with very long arguments and full command visibility"),
            std::string::npos);
}

TEST(ToolPresentationTest, SummonSubagentDoesNotUseRawTaskAsInlineBodyFallback) {
  ToolCallView view;
  view.name = "summon_subagent";
  view.args =
      R"({"title":"Scout","task":"Investigate every failing test path and report the full causal chain without shortening the task text"})";
  view.phase = ToolPhase::Called;

  ToolPresentation p = BuildToolPresentation(view, nullptr, nullptr);
  EXPECT_TRUE(p.body_lines.empty() ||
              p.body_lines.front().find("Investigate every failing test path") ==
                  std::string::npos);
}

TEST(ToolPresentationTest, SearchRowsPreserveFullPrimaryTextWithoutManualEllipsis) {
  ToolCallView view;
  view.name = "grep";
  view.args = R"({"pattern":"needle","path":"src"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"([
    {"path":"src/a.cpp","line_number":4,"line":"needle with long content that should remain visible without manual chopping and ellipsis"}
  ])";

  ToolPresentation p = BuildToolPresentation(view);
  ASSERT_FALSE(p.sections.empty());
  ASSERT_FALSE(p.sections.front().lines.empty());
  EXPECT_EQ(p.sections.front().lines.front().find("..."), std::string::npos);
  EXPECT_NE(p.sections.front().lines.front().find("without manual chopping"),
            std::string::npos);
}

} // namespace
