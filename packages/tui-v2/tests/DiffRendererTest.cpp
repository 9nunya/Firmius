#include "DiffRenderer.hpp"
#include <gtest/gtest.h>

using namespace firmius::tui2;

TEST(DiffRendererTest, BasicUnifiedDiff) {
  std::string diff = "--- a/foo.cpp\n+++ b/foo.cpp\n@@ -1,3 +1,4 @@\n line1\n+added\n line2\n line3";
  auto rendered = DiffRenderer::render(diff, 80);

  EXPECT_GE(rendered.size(), 4u);
  // Should contain the filename
  bool foundFile = false;
  for (const auto& line : rendered) {
    if (line.find("foo.cpp") != std::string::npos) foundFile = true;
  }
  EXPECT_TRUE(foundFile);
}

TEST(DiffRendererTest, AddedLinesPresent) {
  std::string diff = "--- a/foo.cpp\n+++ b/foo.cpp\n@@ -1,2 +1,3 @@\n line1\n+added\n line2";
  auto rendered = DiffRenderer::render(diff, 80);

  bool foundAdded = false;
  for (const auto& line : rendered) {
    if (line.find("added") != std::string::npos) foundAdded = true;
  }
  EXPECT_TRUE(foundAdded);
}

TEST(DiffRendererTest, RemovedLinesPresent) {
  std::string diff = "--- a/foo.cpp\n+++ b/foo.cpp\n@@ -1,3 +1,2 @@\n line1\n-removed\n line2";
  auto rendered = DiffRenderer::render(diff, 80);

  bool foundRemoved = false;
  for (const auto& line : rendered) {
    if (line.find("removed") != std::string::npos) foundRemoved = true;
  }
  EXPECT_TRUE(foundRemoved);
}

TEST(DiffRendererTest, GapCollapsing) {
  // Build a diff with many context lines
  std::string diff = "--- a/foo.cpp\n+++ b/foo.cpp\n@@ -1,12 +1,13 @@\n";
  for (int i = 1; i <= 6; ++i) {
    diff += " line" + std::to_string(i) + "\n";
  }
  diff += "+added\n";
  for (int i = 7; i <= 12; ++i) {
    diff += " line" + std::to_string(i) + "\n";
  }

  auto rendered = DiffRenderer::render(diff, 80);

  bool foundGap = false;
  for (const auto& line : rendered) {
    if (line.find("omitted") != std::string::npos) foundGap = true;
  }
  EXPECT_TRUE(foundGap);
}

TEST(DiffRendererTest, EmptyDiff) {
  auto rendered = DiffRenderer::render("", 80);
  EXPECT_GE(rendered.size(), 1u);
}

TEST(DiffRendererTest, SummaryFooter) {
  std::string diff = "--- a/foo.cpp\n+++ b/foo.cpp\n@@ -1,2 +1,3 @@\n line1\n+added1\n+added2\n line2";
  auto rendered = DiffRenderer::render(diff, 80);

  // Should have a summary line with +2
  bool foundSummary = false;
  for (const auto& line : rendered) {
    if (line.find("+2") != std::string::npos) foundSummary = true;
  }
  EXPECT_TRUE(foundSummary);
}
