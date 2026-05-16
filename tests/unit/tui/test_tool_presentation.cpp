#include "tools/ToolPresentation.hpp"

#include <algorithm>
#include "tools/ArtifactToolPresentation.hpp"
#include "tools/FileToolPresentation.hpp"
#include "tools/GenericToolPresentation.hpp"
#include "tools/McpToolPresentation.hpp"
#include "tools/ProcessToolPresentation.hpp"
#include "tools/PythonToolPresentation.hpp"
#include "tools/SearchToolPresentation.hpp"
#include "tools/SemanticToolPresentation.hpp"
#include "tools/SubagentToolPresentation.hpp"
#include "tools/WebSearchToolPresentation.hpp"
#include <type_traits>
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

TEST(ToolPresentationTest, FamilyBuildersAndMatchersAreExplicitlyEnumerated) {
  static_assert(
      std::is_same_v<decltype(&firmius::tui::BuildArtifactToolPresentation),
                     ToolPresentation (*)(const ToolCallView &)>);
  static_assert(std::is_same_v<decltype(&firmius::tui::BuildFileToolPresentation),
                               ToolPresentation (*)(const ToolCallView &)>);
  static_assert(
      std::is_same_v<decltype(&firmius::tui::BuildGenericToolPresentation),
                     ToolPresentation (*)(const ToolCallView &)>);
  static_assert(std::is_same_v<decltype(&firmius::tui::BuildMcpToolPresentation),
                               ToolPresentation (*)(const ToolCallView &)>);
  static_assert(
      std::is_same_v<decltype(&firmius::tui::BuildProcessToolPresentation),
                     ToolPresentation (*)(const ToolCallView &,
                                          const NormalizedProcessState *)>);
  static_assert(
      std::is_same_v<decltype(&firmius::tui::BuildPythonToolPresentation),
                     ToolPresentation (*)(const ToolCallView &,
                                          const NormalizedProcessState *)>);
  static_assert(std::is_same_v<decltype(&firmius::tui::BuildSearchToolPresentation),
                               ToolPresentation (*)(const ToolCallView &)>);
  static_assert(
      std::is_same_v<decltype(&firmius::tui::BuildSemanticToolPresentation),
                     ToolPresentation (*)(const ToolCallView &)>);
  static_assert(
      std::is_same_v<decltype(&firmius::tui::BuildSubagentToolPresentation),
                     ToolPresentation (*)(const ToolCallView &,
                                          const NormalizedSubagentState *)>);
  static_assert(
      std::is_same_v<decltype(&firmius::tui::BuildTerminateSubagentToolPresentation),
                     ToolPresentation (*)(const ToolCallView &)>);
  static_assert(
      std::is_same_v<decltype(&firmius::tui::BuildWebSearchToolPresentation),
                     ToolPresentation (*)(const ToolCallView &)>);
  static_assert(std::is_same_v<decltype(&firmius::tui::BuildWebFetchToolPresentation),
                               ToolPresentation (*)(const ToolCallView &)>);

  EXPECT_TRUE(firmius::tui::IsFileFamilyTool("file_read"));
  EXPECT_TRUE(firmius::tui::IsFileFamilyTool("Read"));
  EXPECT_TRUE(firmius::tui::IsFileFamilyTool("file_edit"));
  EXPECT_TRUE(firmius::tui::IsFileFamilyTool("Edit"));
  EXPECT_TRUE(firmius::tui::IsFileFamilyTool("file_write"));
  EXPECT_TRUE(firmius::tui::IsFileFamilyTool("list_directory"));
  EXPECT_TRUE(firmius::tui::IsFileFamilyTool("Files"));
  EXPECT_FALSE(firmius::tui::IsFileFamilyTool("web_fetch"));
  EXPECT_FALSE(firmius::tui::IsFileFamilyTool("terminate_subagent"));

  EXPECT_TRUE(firmius::tui::IsSearchFamilyTool("Search"));
  EXPECT_TRUE(firmius::tui::IsSearchFamilyTool("grep"));
  EXPECT_TRUE(firmius::tui::IsSearchFamilyTool("glob"));
  EXPECT_FALSE(firmius::tui::IsSearchFamilyTool("list_directory"));

  EXPECT_TRUE(firmius::tui::IsSemanticFamilyTool("Lsp"));
  EXPECT_TRUE(firmius::tui::IsSemanticFamilyTool("lsp"));
  EXPECT_TRUE(firmius::tui::IsSemanticFamilyTool("lsp_diagnostics"));
  EXPECT_TRUE(firmius::tui::IsSemanticFamilyTool("semantic_search"));
  EXPECT_FALSE(firmius::tui::IsSemanticFamilyTool("mcp_list"));

  EXPECT_TRUE(firmius::tui::IsWebSearchFamilyTool("Web"));
  EXPECT_TRUE(firmius::tui::IsWebSearchFamilyTool("web_search"));
  EXPECT_TRUE(firmius::tui::IsWebSearchFamilyTool("web_fetch"));

  EXPECT_TRUE(firmius::tui::IsSubagentFamilyTool("Delegate"));
  EXPECT_FALSE(firmius::tui::IsSubagentFamilyTool("terminate_subagent"));
  EXPECT_FALSE(firmius::tui::IsSubagentFamilyTool("subagent_terminate"));

  EXPECT_FALSE(firmius::tui::IsMcpFamilyTool("Mcp"));
  EXPECT_FALSE(firmius::tui::IsMcpFamilyTool("mcp_list"));
  EXPECT_TRUE(firmius::tui::IsMcpFamilyTool("mcp__server__tool"));
}

TEST(ToolPresentationTest, CompactDelegateAndWebNamesDispatchThroughFamilyPresenters) {
  ToolCallView delegate;
  delegate.name = "Delegate";
  delegate.args = R"({"action":"Spawn","title":"Auth finder","persona":"glimmer","task":"inspect auth"})";
  delegate.phase = ToolPhase::Called;

  ToolPresentation delegatePresentation = BuildToolPresentation(delegate);
  EXPECT_EQ(delegatePresentation.lifecycle, ToolPresentationLifecycle::Running);

  ToolCallView web;
  web.name = "Web";
  web.args = R"({"action":"Fetch","url":"https://example.com/docs"})";
  web.phase = ToolPhase::Finished;
  web.success = true;
  web.result = R"({"url":"https://example.com/docs","status":200,"content":"ok"})";

  ToolPresentation webPresentation = BuildToolPresentation(web);
  EXPECT_FALSE(webPresentation.footer_badges.empty());
  EXPECT_NE(webPresentation.footer_badges.front().find("example.com"),
            std::string::npos);
}

TEST(ToolPresentationTest, MalformedDelegateDoesNotPretendToBeTermination) {
  ToolCallView delegate;
  delegate.name = "Delegate";
  delegate.args = R"({"task":"inspect repo"})";
  delegate.phase = ToolPhase::Preparing;

  ToolPresentation presentation = BuildToolPresentation(delegate);
  EXPECT_EQ(presentation.title.find("termination"), std::string::npos)
      << presentation.title;
}

TEST(ToolPresentationTest, FailedMalformedDelegateSurfacesErrorText) {
  ToolCallView delegate;
  delegate.name = "Delegate";
  delegate.args = R"({"task":"inspect repo"})";
  delegate.phase = ToolPhase::Error;
  delegate.success = false;
  delegate.result = "Missing required field: action";

  ToolPresentation presentation = BuildToolPresentation(delegate);
  EXPECT_EQ(presentation.title, "subagent call failed");
  ASSERT_TRUE(presentation.error_text.has_value());
  EXPECT_NE(presentation.error_text->find("Missing required field"), std::string::npos);
}

TEST(ToolPresentationTest, CompactReadAndSearchNamesUseSpecializedPresenters) {
  ToolCallView read;
  read.name = "Read";
  read.args = R"({"path":"src/main.cpp"})";
  read.phase = ToolPhase::Preparing;

  ToolPresentation readPresentation = BuildToolPresentation(read);
  EXPECT_EQ(readPresentation.lifecycle, ToolPresentationLifecycle::Preparing);

  ToolCallView search;
  search.name = "Search";
  search.args = R"({"action":"Grep","pattern":"ToolRegistry","path":"packages/core"})";
  search.phase = ToolPhase::Finished;
  search.success = true;
  search.result = R"([{"path":"packages/core/src/tools/ToolRegistry.cpp","line_number":12,"line":"ToolRegistry registry;"}])";

  ToolPresentation searchPresentation = BuildToolPresentation(search);
  const std::string *matches = FindFactValue(searchPresentation, "Matches");
  ASSERT_NE(matches, nullptr);
  EXPECT_EQ(*matches, "1");
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
  EXPECT_FALSE(presentation.expandable);
  ASSERT_EQ(presentation.sections.size(), 1u);
  EXPECT_EQ(presentation.sections[0].title, "Result");
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

TEST(ToolPresentationTest, LongResultShowsAllLinesWithoutTruncation) {
  ToolCallView view;
  view.name = "mystery_tool";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = "line 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\n";

  ToolPresentation presentation = BuildToolPresentation(view);
  EXPECT_FALSE(presentation.expandable);
  EXPECT_FALSE(presentation.expanded);
  ASSERT_EQ(presentation.sections.size(), 1u);
  ASSERT_EQ(presentation.sections[0].lines.size(), 8u);
  EXPECT_EQ(presentation.sections[0].lines.front(), "line 1");
  EXPECT_EQ(presentation.sections[0].lines.back(), "line 8");
  EXPECT_TRUE(presentation.notices.empty());
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
  view.name = "Process";
  view.args = R"({"action":"Execute","command":"sleep 1"})";
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
  view.name = "Process";
  view.args = R"({"action":"Execute","command":"sleep 30"})";
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
  view.name = "Process";
  view.args = R"({"action":"Execute","command":"echo ok"})";
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
  view.name = "Process";
  view.args = R"({"action":"Execute","command":"false"})";
  view.phase = ToolPhase::Finished;
  view.success = false;
  view.result = "exit 1";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Error);
  EXPECT_FALSE(presentation.error_text.has_value());
  ASSERT_GE(presentation.body_lines.size(), 2u);
  EXPECT_EQ(presentation.body_lines.front(), "$ false");
  EXPECT_EQ(presentation.body_lines.back(), "exit 1");
  ASSERT_TRUE(presentation.status_footer.has_value());
  EXPECT_NE(presentation.status_footer.value().find("fail"), std::string::npos);
}

TEST(ToolPresentationTest, ProcessSpawnBackgroundPresentation) {
  ToolCallView view;
  view.name = "Process";
  view.args = R"({"action":"Spawn","command":"tail -f app.log"})";
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
  view.name = "Process";
  view.args = R"({"action":"Wait","process_id":"proc-3","pattern":"READY"})";
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
  view.name = "Process";
  view.args = R"({"action":"Input","process_id":"proc-4","input":"status\n"})";
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
  view.name = "Process";
  view.args = R"({"action":"Status","process_id":"proc-5"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result = R"({"isRunning":false,"exitCode":2,"duration_ms":3210})";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr);
  ASSERT_TRUE(presentation.status_footer.has_value());
  EXPECT_NE(presentation.status_footer.value().find("exit 2"), std::string::npos);
}

TEST(ToolPresentationTest, PythonExecuteUsesProcessFamilyPresentation) {
  ToolCallView view;
  view.name = "Python";
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
  view.name = "Delegate";
  view.args = R"({"action":"Spawn","title":"Worker","task":"Implement login"})";
  view.phase = ToolPhase::Called;

  NormalizedSubagentState state;
  state.parent_tool_call_id = "summon-1";
  state.child_agent_id = "child-1";
  state.child_title = "Worker";
  state.running = true;
  state.wait_state = "running";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr, &state);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Running);
  EXPECT_EQ(presentation.title, "Worker");
  EXPECT_EQ(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "Worker"),
            presentation.footer_badges.end());
  EXPECT_NE(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "running"),
            presentation.footer_badges.end());
}

TEST(ToolPresentationTest, SummonSubagentRetryFallbackPresentation) {
  ToolCallView view;
  view.name = "Delegate";
  view.args = R"({"action":"Spawn","task":"Investigate flakes"})";
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
  base.name = "Delegate";
  base.args = R"({"action":"Spawn","title":"Worker","task":"Task"})";
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
  view.name = "Delegate";
  view.args = R"({"action":"Wait","agent_id":"child-1"})";
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
  EXPECT_EQ(presentation.layout, ToolPresentationLayoutKind::InlineStatusRow);
  EXPECT_TRUE(presentation.body_lines.empty());
  EXPECT_FALSE(presentation.expandable);
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
  view.name = "Delegate";
  view.args = R"({"action":"Spawn","task":"Investigate"})";
  view.phase = ToolPhase::Called;

  NormalizedSubagentState state;
  state.child_agent_id = "child-7";
  state.child_friendly_name = "scout-worker";
  state.running = true;
  state.wait_state = "running";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr, &state);
  EXPECT_EQ(presentation.title, "scout-worker");
  EXPECT_EQ(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "scout-worker"),
            presentation.footer_badges.end());
}

TEST(ToolPresentationTest, WaitUsesFriendlyNameWhenTitleAbsent) {
  ToolCallView view;
  view.name = "Delegate";
  view.args = R"({"action":"Wait","agent_id":"child-8"})";
  view.phase = ToolPhase::Called;

  NormalizedSubagentState state;
  state.child_agent_id = "child-8";
  state.child_friendly_name = "worker-eight";
  state.waiting = true;
  state.wait_state = "waiting";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr, &state);
  EXPECT_EQ(presentation.layout, ToolPresentationLayoutKind::InlineStatusRow);
  EXPECT_NE(presentation.title.find("worker-eight"), std::string::npos);
  EXPECT_EQ(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "worker-eight"),
            presentation.footer_badges.end());
}

TEST(ToolPresentationTest, FallsBackToAgentIdWhenNoTitleOrFriendlyName) {
  ToolCallView view;
  view.name = "Delegate";
  view.args = R"({"action":"Wait","agent_id":"child-9"})";
  view.phase = ToolPhase::Called;

  NormalizedSubagentState state;
  state.child_agent_id = "child-9";
  state.waiting = true;
  state.wait_state = "waiting";

  ToolPresentation presentation = BuildToolPresentation(view, nullptr, &state);
  EXPECT_EQ(presentation.layout, ToolPresentationLayoutKind::InlineStatusRow);
  EXPECT_NE(presentation.title.find("child-9"), std::string::npos);
  EXPECT_EQ(std::find(presentation.footer_badges.begin(), presentation.footer_badges.end(),
                      "child-9"),
            presentation.footer_badges.end());
}

TEST(ToolPresentationTest, ProcessExecuteAnsiPresentation) {
  ToolCallView view;
  view.name = "Process";
  view.args = R"({"action":"Execute","command":"ls"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  // \u001b[32m is green
  view.result = R"({"stdout":"\u001b[32mfile.txt\u001b[0m\n","exit_code":0})";

  ToolPresentation presentation = BuildToolPresentation(view);
  EXPECT_TRUE(presentation.ansi_aware);
  bool found = false;
  for (const auto &line : presentation.body_lines) {
    if (line.find("\x1b[32mfile.txt") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST(ToolPresentationTest, ArtifactWriteSuccessPresentationHasReferenceAndPreview) {
  ToolCallView view;
  view.name = "Artifacts";
  view.args = R"({"action":"Write","name":"REPORT.md","kind":"report","content":"line1\nline2\nline3"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result =
      R"({"status":"created","created":true,"updated":false,"reference":"@artifact:lead/REPORT.md","artifact":{"filename":"REPORT.md","owner_friendly_name":"lead","kind":"report","description":"Weekly report"}})";

  ToolPresentation presentation = BuildToolPresentation(view);
  EXPECT_EQ(presentation.lifecycle, ToolPresentationLifecycle::Success);
  EXPECT_EQ(presentation.layout, ToolPresentationLayoutKind::DiffPreview);
  const std::string *reference = FindFactValue(presentation, "Reference");
  ASSERT_NE(reference, nullptr);
  EXPECT_EQ(*reference, "@artifact:lead/REPORT.md");
  ASSERT_EQ(presentation.diff_sections.size(), 1u);
  ASSERT_EQ(presentation.diff_sections.front().lines.size(), 3u);
  EXPECT_TRUE(presentation.diff_sections.front().lines.front().highlight_background);
}

TEST(ToolPresentationTest, ArtifactWriteUpdatePresentationUsesStoredPreviousContent) {
  ToolCallView view;
  view.name = "Artifacts";
  view.args = R"({"action":"Write","name":"REPORT.md","kind":"report","content":"line1\nline2 changed\nline3\nline4"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result =
      R"({"status":"updated","created":false,"updated":true,"reference":"@artifact:lead/REPORT.md","previous_content":"line1\nline2\nline3\n","artifact":{"filename":"REPORT.md","owner_friendly_name":"lead","kind":"report","description":"Weekly report"}})";

  ToolPresentation presentation = BuildToolPresentation(view);
  EXPECT_EQ(presentation.layout, ToolPresentationLayoutKind::DiffPreview);
  ASSERT_EQ(presentation.diff_sections.size(), 1u);
  ASSERT_EQ(presentation.diff_sections.front().lines.size(), 3u);
  EXPECT_EQ(presentation.diff_sections.front().lines[0].type, '-');
  EXPECT_EQ(presentation.diff_sections.front().lines[1].type, '+');
  EXPECT_EQ(presentation.diff_sections.front().lines[2].type, '+');
}

TEST(ToolPresentationTest, ArtifactReadPreparingAndErrorStatesAreExplicit) {
  ToolCallView preparing;
  preparing.name = "Artifacts";
  preparing.args = R"({"action":"Read","reference":"@artifact:lead/REPORT.md"})";
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
  view.name = "Artifacts";
  view.args = R"({"action":"List"})";
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

TEST(ToolPresentationTest, TodoWriteShowsPatchCountsAndItems) {
  ToolCallView view;
  view.name = "Todo";
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
  view.name = "Artifacts";
  view.args = R"({"action":"Read","reference":"@artifact:worker/out.md"})";
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
  view.name = "Todo";
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
  view.name = "Todo";
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
  read.result =
      R"({"line_start":1,"line_end":13,"lines_read":13,"watch_state":"updated","watch_scope":"full"})";
  ToolPresentation r = BuildToolPresentation(read);
  EXPECT_EQ(r.lifecycle, ToolPresentationLifecycle::Success);
  EXPECT_FALSE(r.expandable);
  EXPECT_TRUE(r.sections.empty());
  const std::string *watch_scope = FindFactValue(r, "Watch scope");
  ASSERT_NE(watch_scope, nullptr);
  EXPECT_EQ(*watch_scope, "full file");

  ToolCallView edit;
  edit.name = "file_edit";
  edit.args = R"({"path":"src/main.cpp","edits":[{"op":"replace_range"}]})";
  edit.phase = ToolPhase::Called;
  ToolPresentation e_running = BuildToolPresentation(edit);
  EXPECT_EQ(e_running.lifecycle, ToolPresentationLifecycle::Running);
  EXPECT_EQ(e_running.title, "edited main.cpp");

  edit.phase = ToolPhase::Finished;
  edit.success = true;
  edit.result = R"({"operations":[{"op":"replace_range","description":"replace line","old_lines":["a"],"new_lines":["b"]}]})";
  ToolPresentation e = BuildToolPresentation(edit);
  EXPECT_EQ(e.layout, ToolPresentationLayoutKind::DiffPreview);
  EXPECT_NE(e.title.find("main.cpp"), std::string::npos);
  ASSERT_TRUE(e.custom_icon.has_value());
  const std::string *adds = FindFactValue(e, "Added lines");
  const std::string *removes = FindFactValue(e, "Removed lines");
  ASSERT_NE(adds, nullptr);
  ASSERT_NE(removes, nullptr);
  EXPECT_EQ(*adds, "1");
  EXPECT_EQ(*removes, "1");
  ASSERT_EQ(e.diff_sections.size(), 1u);
  EXPECT_TRUE(e.diff_sections.front().title.empty());
  ASSERT_EQ(e.diff_sections.front().lines.size(), 2u);
  EXPECT_EQ(e.diff_sections.front().lines[0].type, '-');
  EXPECT_EQ(e.diff_sections.front().lines[1].type, '+');

  ToolCallView overwrite_edit;
  overwrite_edit.name = "file_edit";
  overwrite_edit.args = R"({"path":"src/main.cpp","content":"line 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\nline 9\n"})";
  overwrite_edit.phase = ToolPhase::Finished;
  overwrite_edit.success = true;
  overwrite_edit.result = R"({"mode":"overwrite"})";
  ToolPresentation overwrite = BuildToolPresentation(overwrite_edit);
  EXPECT_EQ(overwrite.layout, ToolPresentationLayoutKind::DiffPreview);
  ASSERT_EQ(overwrite.diff_sections.size(), 1u);
  EXPECT_TRUE(overwrite.diff_sections.front().title.empty());
  ASSERT_EQ(overwrite.diff_sections.front().lines.size(), 9u);
  EXPECT_EQ(overwrite.diff_sections.front().lines.front().type, '+');
  EXPECT_TRUE(overwrite.diff_sections.front().lines.front().highlight_background);

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

TEST(ToolPresentationTest, MultiFileFileEditRendersSeparateDiffSections) {
  ToolCallView edit;
  edit.name = "file_edit";
  edit.args =
      R"({"files":[{"path":"src/a.cpp","edits":[{"op":"insert_after"}]},{"path":"src/b.cpp","edits":[{"op":"replace_range"}]}]})";
  edit.phase = ToolPhase::Finished;
  edit.success = true;
  edit.result =
      R"({"mode":"multi_file","files":[{"path":"src/a.cpp","operations":[{"op":"insert_after","description":"insert after 1#aaaa","start_line":2,"end_line":2,"old_lines":[],"new_lines":["alpha"]}]},{"path":"src/b.cpp","operations":[{"op":"replace_range","description":"replace line","start_line":3,"end_line":3,"old_lines":["old"],"new_lines":["new"]}]}]})";

  ToolPresentation p = BuildToolPresentation(edit);
  ASSERT_EQ(p.diff_sections.size(), 2u);
  EXPECT_EQ(p.diff_sections[0].title, "src/a.cpp");
  EXPECT_EQ(p.diff_sections[1].title, "src/b.cpp");
  EXPECT_NE(p.diff_sections[0].meta.find("1 change"), std::string::npos);
  EXPECT_NE(p.diff_sections[1].meta.find("1 change"), std::string::npos);
  const std::string *adds = FindFactValue(p, "Added lines");
  const std::string *removes = FindFactValue(p, "Removed lines");
  ASSERT_NE(adds, nullptr);
  ASSERT_NE(removes, nullptr);
  EXPECT_EQ(*adds, "2");
  EXPECT_EQ(*removes, "1");
}

TEST(ToolPresentationTest, MultiFileFileEditGroupsMultipleOperationsByFile) {
  ToolCallView edit;
  edit.name = "file_edit";
  edit.args =
      R"({"files":[{"path":"src/a.cpp","edits":[{"op":"insert_after"},{"op":"replace_range"}]},{"path":"src/b.cpp","edits":[{"op":"delete_range"}]}]})";
  edit.phase = ToolPhase::Finished;
  edit.success = true;
  edit.result =
      R"({"mode":"multi_file","files":[{"path":"src/a.cpp","operations":[{"op":"insert_after","description":"insert after 1#aaaa","start_line":2,"end_line":2,"old_lines":[],"new_lines":["alpha"]},{"op":"replace_range","description":"replace 3#bbbb...4#cccc","start_line":3,"end_line":4,"old_lines":["old1","old2"],"new_lines":["new1","new2"]}]},{"path":"src/b.cpp","operations":[{"op":"delete_range","description":"delete 8#dddd...9#eeee","start_line":8,"end_line":9,"old_lines":["x","y"],"new_lines":[]}]}]})";

  ToolPresentation p = BuildToolPresentation(edit);
  ASSERT_EQ(p.diff_sections.size(), 2u);
  EXPECT_EQ(p.diff_sections[0].title, "src/a.cpp");
  EXPECT_NE(p.diff_sections[0].meta.find("2 changes"), std::string::npos);
  EXPECT_NE(p.diff_sections[0].meta.find("insert after"), std::string::npos);
  EXPECT_NE(p.diff_sections[0].meta.find("replace range"), std::string::npos);
  EXPECT_EQ(p.diff_sections[0].meta.find("1#aaaa"), std::string::npos);
  EXPECT_EQ(p.diff_sections[0].meta.find("3#bbbb"), std::string::npos);
  EXPECT_EQ(p.diff_sections[1].title, "src/b.cpp");
}

TEST(ToolPresentationTest,
     FileEditFallsBackToDurableEditedFileSummaryWhenDiffDetailsMissing) {
  ToolCallView edit;
  edit.name = "file_edit";
  edit.args = R"({"path":"src/main.cpp"})";
  edit.phase = ToolPhase::Finished;
  edit.success = true;
  edit.result = R"({"path":"src/main.cpp"})";
  edit.fileEditEvents.push_back(
      {"src/main.cpp", "", 3, 1});

  ToolPresentation p = BuildToolPresentation(edit);
  EXPECT_NE(p.title.find("main.cpp"), std::string::npos);
  ASSERT_EQ(p.diff_sections.size(), 1u);
  EXPECT_TRUE(p.diff_sections.front().lines.empty());
  ASSERT_TRUE(p.diff_sections.front().empty_state_text.has_value());
  EXPECT_NE(p.diff_sections.front().empty_state_text.value().find("diff preview unavailable"),
            std::string::npos);
  const std::string *adds = FindFactValue(p, "Added lines");
  const std::string *removes = FindFactValue(p, "Removed lines");
  ASSERT_NE(adds, nullptr);
  ASSERT_NE(removes, nullptr);
  EXPECT_EQ(*adds, "3");
  EXPECT_EQ(*removes, "1");
}

TEST(ToolPresentationTest,
     FileWriteFallsBackToDurableEditedFileSummaryWhenDiffDetailsMissing) {
  ToolCallView edit;
  edit.name = "file_write";
  edit.args = R"({"path":"src/write.cpp","content":"hello\n"})";
  edit.phase = ToolPhase::Finished;
  edit.success = true;
  edit.result = R"({"path":"src/write.cpp","added_lines":1,"removed_lines":0})";
  edit.fileEditEvents.push_back(
      {"src/write.cpp", "", 1, 0});

  ToolPresentation p = BuildToolPresentation(edit);
  EXPECT_NE(p.title.find("write.cpp"), std::string::npos);
  ASSERT_EQ(p.diff_sections.size(), 1u);
  EXPECT_FALSE(p.diff_sections.front().lines.empty());
  EXPECT_FALSE(p.diff_sections.front().empty_state_text.has_value());
  const std::string *adds = FindFactValue(p, "Added lines");
  ASSERT_NE(adds, nullptr);
  EXPECT_EQ(*adds, "1");
}

TEST(ToolPresentationTest, RunningFileEditShowsEditedPathAndWaitingDiagnostics) {
  ToolCallView edit;
  edit.name = "file_edit";
  edit.args = R"({"path":"src/main.cpp","edits":[{"op":"replace_range"}]})";
  edit.phase = ToolPhase::Called;
  edit.success = true;
  edit.fileEditEvents.push_back(
      {"src/main.cpp", "", 1, 1});

  ToolPresentation p = BuildToolPresentation(edit);
  EXPECT_EQ(p.lifecycle, ToolPresentationLifecycle::Running);
  EXPECT_NE(p.title.find("main.cpp"), std::string::npos);
  EXPECT_EQ(p.title, "edited main.cpp");
  ASSERT_FALSE(p.notices.empty());
  EXPECT_NE(p.notices.front().text.find("Running diagnostics"), std::string::npos);
  ASSERT_EQ(p.diff_sections.size(), 1u);
  EXPECT_TRUE(p.diff_sections.front().lines.empty());
  EXPECT_TRUE(p.diff_sections.front().empty_state_text.has_value());
}

TEST(ToolPresentationTest, RunningFileEditShowsLiveDiffWhileDiagnosticsPending) {
  ToolCallView edit;
  edit.name = "file_edit";
  edit.args =
      R"({"path":"src/main.cpp","edits":[{"op":"replace_range","start_anchor":"12","end_anchor":"12","new_lines":["new value"]}]})";
  edit.phase = ToolPhase::Called;
  edit.success = true;
  edit.fileEditEvents.push_back(
      {"src/main.cpp", "@@ replace range @@\n-old value\n+new value\n", 1, 1});

  ToolPresentation p = BuildToolPresentation(edit);
  EXPECT_EQ(p.lifecycle, ToolPresentationLifecycle::Running);
  ASSERT_EQ(p.diff_sections.size(), 1u);
  ASSERT_EQ(p.diff_sections.front().lines.size(), 2u);
  EXPECT_EQ(p.diff_sections.front().lines[0].type, '-');
  EXPECT_EQ(p.diff_sections.front().lines[0].content, "old value");
  EXPECT_EQ(p.diff_sections.front().lines[1].type, '+');
  EXPECT_EQ(p.diff_sections.front().lines[1].content, "new value");
}

TEST(ToolPresentationTest,
     RunningPatchOnlyEditShowsPatchPreviewWhileDiagnosticsPending) {
  ToolCallView edit;
  edit.name = "Edit";
  edit.args =
      R"({"patch":"--- a/src/main.cpp\n+++ b/src/main.cpp\n@@ -12,1 +12,1 @@\n-old value\n+new value"})";
  edit.phase = ToolPhase::Called;
  edit.success = true;

  ToolPresentation p = BuildToolPresentation(edit);
  EXPECT_EQ(p.lifecycle, ToolPresentationLifecycle::Running);
  EXPECT_NE(p.title.find("main.cpp"), std::string::npos);
  ASSERT_EQ(p.diff_sections.size(), 1u);
  ASSERT_EQ(p.diff_sections.front().lines.size(), 2u);
  EXPECT_EQ(p.diff_sections.front().lines[0].type, '-');
  EXPECT_EQ(p.diff_sections.front().lines[0].content, "old value");
  EXPECT_EQ(p.diff_sections.front().lines[1].type, '+');
  EXPECT_EQ(p.diff_sections.front().lines[1].content, "new value");
}

TEST(ToolPresentationTest,
     FinishedPatchOnlyEditPrefersPatchPreviewOverSummaryOnlyFallback) {
  ToolCallView edit;
  edit.name = "Edit";
  edit.args =
      R"({"patch":"--- a/src/main.cpp\n+++ b/src/main.cpp\n@@ -12,1 +12,1 @@\n-old value\n+new value"})";
  edit.phase = ToolPhase::Finished;
  edit.success = true;
  edit.result = R"({"resolved_mode":"patch","path":"src/main.cpp","added_lines":1,"removed_lines":1})";
  edit.fileEditEvents.push_back({"src/main.cpp", "", 1, 1});

  ToolPresentation p = BuildToolPresentation(edit);
  ASSERT_EQ(p.diff_sections.size(), 1u);
  EXPECT_FALSE(p.diff_sections.front().lines.empty());
  EXPECT_FALSE(p.diff_sections.front().empty_state_text.has_value());
}

TEST(ToolPresentationTest, RunningMultiFileEditShowsEditedFilesBeforeFinalResult) {
  ToolCallView edit;
  edit.name = "file_edit";
  edit.args =
      R"({"files":[{"path":"src/a.cpp"},{"path":"src/b.cpp"}]})";
  edit.phase = ToolPhase::Called;
  edit.success = true;
  edit.fileEditEvents.push_back({"src/a.cpp", "", 1, 1});
  edit.fileEditEvents.push_back({"src/b.cpp", "", 1, 0});

  ToolPresentation p = BuildToolPresentation(edit);
  EXPECT_EQ(p.lifecycle, ToolPresentationLifecycle::Running);
  EXPECT_EQ(p.title, "edited 2 files");
  ASSERT_FALSE(p.notices.empty());
  EXPECT_NE(p.notices.front().text.find("Running diagnostics"), std::string::npos);
  ASSERT_EQ(p.diff_sections.size(), 2u);
}

TEST(ToolPresentationTest, SearchReplaceFileEditRendersDiffPreview) {
  ToolCallView edit;
  edit.name = "file_edit";
  edit.args =
      R"({"path":"src/main.cpp","edits":[{"op":"search_replace","old_string":"alpha","new_string":"omega","replace_all":true}]})";
  edit.phase = ToolPhase::Finished;
  edit.success = true;
  edit.result =
      R"({"path":"src/main.cpp","mode":"search_replace_edits","replacements":2,"applied_edits":1,"added_lines":2,"removed_lines":2,"diff_preview":"@@ search_replace alpha @@\n-alpha beta\n-beta alpha\n+omega beta\n+beta omega\n","operations":[{"op":"search_replace","description":"search_replace alpha","start_line":1,"end_line":2,"new_line_count":2,"old_line_count":2,"relocated":false,"old_lines":["alpha beta","beta alpha"],"new_lines":["omega beta","beta omega"]}],"watch_state":"refreshed"})";

  ToolPresentation p = BuildToolPresentation(edit);
  ASSERT_EQ(p.diff_sections.size(), 1u);
  ASSERT_EQ(p.diff_sections.front().lines.size(), 4u);
  EXPECT_EQ(p.diff_sections.front().lines[0].type, '-');
  EXPECT_EQ(p.diff_sections.front().lines[1].type, '-');
  EXPECT_EQ(p.diff_sections.front().lines[2].type, '+');
  EXPECT_EQ(p.diff_sections.front().lines[3].type, '+');
  const std::string *adds = FindFactValue(p, "Added lines");
  const std::string *removes = FindFactValue(p, "Removed lines");
  ASSERT_NE(adds, nullptr);
  ASSERT_NE(removes, nullptr);
  EXPECT_EQ(*adds, "2");
  EXPECT_EQ(*removes, "2");
}


TEST(ToolPresentationTest, FileEditDiagnosticsMoveOutOfFooterIntoSections) {
  ToolCallView edit;
  edit.name = "file_edit";
  edit.args = R"({"path":"src/main.cpp","edits":[{"op":"replace_range"}]})";
  edit.phase = ToolPhase::Finished;
  edit.success = true;
  edit.result = R"({"operations":[{"op":"replace_range","description":"replace line","old_lines":["a"],"new_lines":["b"]}],"lsp":{"checked":true,"available":true,"server_id":"clangd","errors":1,"warnings":2,"new_error_count":0,"new_warning_count":1,"new_warnings":["WARN [4:2] unused variable"]}})";

  ToolPresentation p = BuildToolPresentation(edit);
  EXPECT_EQ(std::find(p.footer_badges.begin(), p.footer_badges.end(), "lsp:W2"),
            p.footer_badges.end());
  EXPECT_EQ(std::find(p.footer_badges.begin(), p.footer_badges.end(), "new:W1"),
            p.footer_badges.end());
  ASSERT_FALSE(p.sections.empty());
  EXPECT_NE(p.sections.back().title.find("New diagnostics"), std::string::npos);
  EXPECT_NE(p.sections.back().lines.front().find("WARN [4:2] unused variable"),
            std::string::npos);
}

TEST(ToolPresentationTest, MultiFilePatchRendersSeparateDiffSectionsFromResults) {
  ToolCallView edit;
  edit.name = "file_edit";
  edit.args =
      R"({"files":[{"path":"src/a.cpp","patch":"@@ 1 @@\n-old\n+new\n"},{"path":"src/b.cpp","patch":"@@ 2 @@\n-beta\n+gamma\n"}]})";
  edit.phase = ToolPhase::Finished;
  edit.success = true;
  edit.result =
      R"({"mode":"multi_file","files":[{"path":"src/a.cpp","mode":"patch","operations":[{"op":"replace_range","description":"replace line","start_line":1,"end_line":1,"old_lines":["old"],"new_lines":["new"]}]},{"path":"src/b.cpp","mode":"patch","operations":[{"op":"replace_range","description":"replace line","start_line":2,"end_line":2,"old_lines":["beta"],"new_lines":["gamma"]}]}]})";

  ToolPresentation p = BuildToolPresentation(edit);
  ASSERT_EQ(p.diff_sections.size(), 2u);
  EXPECT_EQ(p.diff_sections[0].title, "src/a.cpp");
  EXPECT_EQ(p.diff_sections[1].title, "src/b.cpp");
  EXPECT_EQ(p.diff_sections[0].lines.size(), 2u);
  EXPECT_EQ(p.diff_sections[1].lines.size(), 2u);
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
  term.name = "Delegate";
  term.args = R"({"action":"Stop","agent_id":"child-1"})";
  term.phase = ToolPhase::Finished;
  term.success = true;
  term.result = R"({"agent_id":"child-1","status":"terminated"})";
  ToolPresentation t = BuildToolPresentation(term, nullptr, nullptr);
  const std::string *subagent = FindFactValue(t, "Subagent");
  const std::string *status = FindFactValue(t, "Status");
  ASSERT_NE(subagent, nullptr);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(*subagent, "child-1");
  EXPECT_EQ(*status, "terminated");
}

TEST(ToolPresentationTest, LongProcessCommandIsNotDestructivelyTruncated) {
  ToolCallView view;
  view.name = "Process";
  view.args = R"({"action":"Execute","command":"python script.py --with very long arguments and full command visibility"})";
  view.phase = ToolPhase::Called;

  ToolPresentation p = BuildToolPresentation(view, nullptr);
  ASSERT_FALSE(p.body_lines.empty());
  EXPECT_NE(p.body_lines.front().find("$ python script.py --with very long arguments and full command visibility"),
            std::string::npos);
}

TEST(ToolPresentationTest, SummonSubagentDoesNotUseRawTaskAsInlineBodyFallback) {
  ToolCallView view;
  view.name = "Delegate";
  view.args =
      R"({"action":"Spawn","title":"Scout","task":"Investigate every failing test path and report the full causal chain without shortening the task text"})";
  view.phase = ToolPhase::Called;

  ToolPresentation p = BuildToolPresentation(view, nullptr, nullptr);
  EXPECT_TRUE(p.body_lines.empty());
}

TEST(ToolPresentationTest, SummonSubagentUsesCuratedSummaryPreviewLine) {
  ToolCallView view;
  view.name = "Delegate";
  view.args = R"({"action":"Spawn","title":"Worker","task":"Implement login"})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.result =
      R"({"agentId":"child-1","status":"completed","result":"done line 1\ndone line 2\ndone line 3"})";

  ToolPresentation p = BuildToolPresentation(view, nullptr, nullptr);
  ASSERT_EQ(p.body_lines.size(), 1u);
  EXPECT_NE(p.body_lines.front().find("summary: done line 1"), std::string::npos);
  EXPECT_EQ(p.body_lines.front().find("done line 2"), std::string::npos);
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


TEST(ToolPresentationTest, McpDynamicToolDecodesServerAndToolIdentity) {
  ToolCallView view;
  view.name = "mcp__server_x2D_one__search_x2F_docs";
  view.args = R"({"arguments":{"q":"hello"}})";
  view.phase = ToolPhase::Finished;
  view.success = true;
  view.show_result = false;
  view.result = R"({"remote_result":[{"id":1}]})";

  ToolPresentation presentation = BuildToolPresentation(view);
  EXPECT_NE(presentation.title.find("MCP tool search/docs @ server-one"),
            std::string::npos);
  const std::string *server = FindFactValue(presentation, "Server");
  const std::string *tool = FindFactValue(presentation, "Tool");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(tool, nullptr);
  EXPECT_EQ(*server, "server-one");
  EXPECT_EQ(*tool, "search/docs");
}
} // namespace
