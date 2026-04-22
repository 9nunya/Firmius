#include "components/TranscriptGrouping.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;
using firmius::tui::DescribeQuickToolCall;
using firmius::tui::QuickToolCategory;
using firmius::tui::QuickToolGroupLabel;
using firmius::tui::QuickToolGroupSummary;

TEST(TranscriptGroupingTest, DescribesFileReadsCompactly) {
  ToolCallView view;
  view.name = "Files";
  view.args =
      R"({"action":"Read","path":"/mnt/SHIT/Projects/Firmius/src/main.cpp","start_line":12,"end_line":28})";

  auto descriptor = DescribeQuickToolCall(view);
  EXPECT_EQ(descriptor.category, QuickToolCategory::Read);
  EXPECT_EQ(descriptor.target, "src/main.cpp:12-28");
}

TEST(TranscriptGroupingTest, DescribesSearchesWithPatternAndPath) {
  ToolCallView view;
  view.name = "Search";
  view.args = R"({"action":"Grep","pattern":"NativeCompiler|emit","path":"src/compiler"})";

  auto descriptor = DescribeQuickToolCall(view);
  EXPECT_EQ(descriptor.category, QuickToolCategory::Search);
  EXPECT_EQ(descriptor.target, "\"NativeCompiler|emit\" in src/compiler");
}

TEST(TranscriptGroupingTest, DescribesDirectoryListingsCompactly) {
  ToolCallView view;
  view.name = "Files";
  view.args = R"({"action":"List","path":"/mnt/SHIT/Projects/Firmius/src/tools"})";

  auto descriptor = DescribeQuickToolCall(view);
  EXPECT_EQ(descriptor.category, QuickToolCategory::List);
  EXPECT_EQ(descriptor.target, "src/tools");
}

TEST(TranscriptGroupingTest, UnnamedQuickToolRowsDoNotProduceDescriptors) {
  ToolCallView view;
  view.name = "   ";
  view.args = R"({"path":"src/main.cpp","start_line":1,"end_line":5})";

  auto descriptor = DescribeQuickToolCall(view);
  EXPECT_EQ(descriptor.category, QuickToolCategory::None);
  EXPECT_TRUE(descriptor.target.empty());

  view.name = "Files";
  view.args = R"({"action":"Read","path":"src/main.cpp","start_line":1,"end_line":5})";
  descriptor = DescribeQuickToolCall(view);
  EXPECT_EQ(descriptor.category, QuickToolCategory::List);
}

TEST(TranscriptGroupingTest, GroupLabelDeduplicatesTargets) {
  QuickToolGroupSummary summary;
  summary.category = QuickToolCategory::Read;
  summary.targets = {"src/a.cpp", "src/b.cpp", "src/a.cpp"};
  summary.has_live = true;

  EXPECT_EQ(QuickToolGroupLabel(summary), "Read src/a.cpp, src/b.cpp");
}

TEST(TranscriptGroupingTest, DedupeTargetsPreservesOrder) {
  std::vector<std::string> targets = {"a", "b", "a", "c", "b"};
  auto deduped = firmius::tui::DedupeQuickToolTargets(targets);

  ASSERT_EQ(deduped.size(), 3u);
  EXPECT_EQ(deduped[0], "a");
  EXPECT_EQ(deduped[1], "b");
  EXPECT_EQ(deduped[2], "c");
}

} // namespace
