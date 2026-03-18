#include "components/FileEditDiff.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

namespace firmius::tui {

class DiffRenderingTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(DiffRenderingTest, BuildDiffHunksTracksOldAndNewLineNumbers) {
  auto hunks =
      BuildDiffHunks("alpha\nbeta\ngamma\n", "alpha\nbeta2\ngamma\ndelta\n");

  ASSERT_EQ(hunks.size(), 2u);
  ASSERT_EQ(hunks[0].lines.size(), 2u);
  EXPECT_EQ(hunks[0].lines[0].type, '-');
  EXPECT_EQ(hunks[0].lines[0].oldLine, 2);
  EXPECT_EQ(hunks[0].lines[0].newLine, 0);
  EXPECT_EQ(hunks[0].lines[1].type, '+');
  EXPECT_EQ(hunks[0].lines[1].oldLine, 0);
  EXPECT_EQ(hunks[0].lines[1].newLine, 2);
  ASSERT_EQ(hunks[1].lines.size(), 1u);
  EXPECT_EQ(hunks[1].lines[0].type, '+');
  EXPECT_EQ(hunks[1].lines[0].newLine, 4);
}

TEST_F(DiffRenderingTest, CountDiffStatsCountsOnlyActualChanges) {
  auto hunks =
      BuildDiffHunks("same\nkeep\nold\n", "same\nkeep\nnew\nplus\n");
  auto stats = CountDiffStats(hunks);

  EXPECT_EQ(stats.removed, 1);
  EXPECT_EQ(stats.added, 2);
}

TEST_F(DiffRenderingTest, HunksWithMoreChangesAreMoreRelevant) {
  std::vector<DiffHunk> hunks = {
      DiffHunk{1, 1, 1, 0, {{'-', 1, 0, "a"}}},
      DiffHunk{5, 3, 5, 4,
               {{'-', 5, 0, "x"}, {'-', 6, 0, "y"}, {'+', 0, 5, "x2"},
                {'+', 0, 6, "y2"}, {'+', 0, 7, "z2"}}},
  };

  auto ranked = RankHunksByRelevance(hunks);
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked[0], 1u);
}

TEST_F(DiffRenderingTest, SeparateAdditionAtEndUsesNewLineNumbersOnly) {
  auto hunks = BuildDiffHunks("one\ntwo\n", "one\ntwo\nthree\n");

  ASSERT_EQ(hunks.size(), 1u);
  ASSERT_EQ(hunks[0].lines.size(), 1u);
  EXPECT_EQ(hunks[0].lines[0].type, '+');
  EXPECT_EQ(hunks[0].lines[0].oldLine, 0);
  EXPECT_EQ(hunks[0].lines[0].newLine, 3);
}

} // namespace firmius::tui
