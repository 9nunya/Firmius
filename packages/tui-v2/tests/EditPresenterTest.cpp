#include "tools/EditPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "utils/ToolView.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(EditPresenterTest, MatchesAllEditTools) {
  EditPresenter p;
  EXPECT_TRUE(p.matches("Edit"));
  EXPECT_TRUE(p.matches("EditWrite"));
  EXPECT_TRUE(p.matches("EditReplace"));
  EXPECT_TRUE(p.matches("EditRange"));
  EXPECT_FALSE(p.matches("Process"));
}

TEST(EditPresenterTest, PreparingPhase) {
  EditPresenter p;
  ToolCallItem item("call-1", "Edit", "agent-1");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
}

TEST(EditPresenterTest, CalledShowsLoadingDiff) {
  EditPresenter p;
  ToolCallItem item("call-1", "Edit", "agent-1");
  item.setArgs(R"({"patch":"--- a/foo.cpp\n+++ b/foo.cpp\n"})");
  item.setPhase(ToolPhase::Called);
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 2u);
  bool foundLoading = false;
  for (const auto& line : lines) {
    if (line.find("Loading diff") != std::string::npos) foundLoading = true;
  }
  EXPECT_TRUE(foundLoading);
}

TEST(EditPresenterTest, CalledWithDiffAlreadyPresent) {
  EditPresenter p;
  ToolCallItem item("call-1", "Edit", "agent-1");
  item.setArgs(R"({"patch":"--- a/foo.cpp\n+++ b/foo.cpp\n"})");
  item.setPhase(ToolPhase::Called);

  firmius::shared::FileEditSignal signal;
  signal.path = "foo.cpp";
  signal.diffPreview = "--- a/foo.cpp\n+++ b/foo.cpp\n@@ -1,2 +1,3 @@\n line1\n+added\n line2";
  signal.addedLines = 1;
  signal.removedLines = 0;
  item.addDiffEdit(signal);

  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 2u);
  bool foundDiff = false;
  for (const auto& line : lines) {
    if (line.find("foo.cpp") != std::string::npos) foundDiff = true;
  }
  EXPECT_TRUE(foundDiff);
}

TEST(EditPresenterTest, FinishedSuccess) {
  EditPresenter p;
  ToolCallItem item("call-1", "Edit", "agent-1");
  item.setArgs(R"({"patch":"..."})");

  firmius::shared::FileEditSignal signal;
  signal.path = "foo.cpp";
  signal.diffPreview = "--- a/foo.cpp\n+++ b/foo.cpp\n@@ -1,2 +1,3 @@\n line1\n+added\n line2";
  signal.addedLines = 1;
  signal.removedLines = 0;
  item.addDiffEdit(signal);

  item.setResult(true, R"({"files_changed":1,"total_added":1,"total_removed":0})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  bool foundSuccess = false;
  for (const auto& line : lines) {
    if (line.find("\xe2\x9c\x93") != std::string::npos) foundSuccess = true;
  }
  EXPECT_TRUE(foundSuccess);
}

TEST(EditPresenterTest, FinishedError) {
  EditPresenter p;
  ToolCallItem item("call-1", "Edit", "agent-1");
  item.setArgs(R"({"patch":"..."})");
  item.setResult(false, R"({"error":"Hunk mismatch"})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  bool foundError = false;
  for (const auto& line : lines) {
    if (line.find("\xe2\x9c\x97") != std::string::npos) foundError = true;
  }
  EXPECT_TRUE(foundError);
}

TEST(EditPresenterTest, MultiFileDiff) {
  EditPresenter p;
  ToolCallItem item("call-1", "Edit", "agent-1");
  item.setArgs(R"({"patch":"..."})");

  firmius::shared::FileEditSignal s1;
  s1.path = "foo.cpp";
  s1.diffPreview = "--- a/foo.cpp\n+++ b/foo.cpp\n@@ -1,2 +1,3 @@\n line1\n+added\n line2";
  s1.addedLines = 1;
  s1.removedLines = 0;
  item.addDiffEdit(s1);

  firmius::shared::FileEditSignal s2;
  s2.path = "bar.cpp";
  s2.diffPreview = "--- a/bar.cpp\n+++ b/bar.cpp\n@@ -1,2 +1,3 @@\n line1\n+added2\n line2";
  s2.addedLines = 1;
  s2.removedLines = 0;
  item.addDiffEdit(s2);

  item.setResult(true, R"({"files_changed":2})");
  auto lines = p.render(item, {}, 80);
  ASSERT_GE(lines.size(), 1u);
  // Should mention 2 files
  bool foundMultiFile = false;
  for (const auto& line : lines) {
    if (line.find("2 files") != std::string::npos) foundMultiFile = true;
  }
  EXPECT_TRUE(foundMultiFile);
}
