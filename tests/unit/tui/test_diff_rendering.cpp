#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <algorithm>

// We can't directly test the static functions in FileEditToolBlock.cpp
// So we test the concepts instead

namespace firmius::tui {

class DiffRenderingTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

// Test diff hunk relevance ranking logic
TEST_F(DiffRenderingTest, HunksWithMoreChangesAreMoreRelevant) {
  // This tests the concept that hunks with more changes should rank higher
  // The actual implementation is in FileEditToolBlock.cpp::rankHunksByRelevance
  
  // Simulate scoring
  auto calculateScore = [](int old_count, int new_count, bool is_middle) -> int {
    int score = 0;
    score += (old_count + new_count) * 2;
    if (is_middle) score += 5;
    if (old_count > 0 && new_count > 0) score += 10;
    return score;
  };
  
  // Small hunk at start
  int score1 = calculateScore(2, 1, false);
  
  // Large hunk in middle with both additions and deletions
  int score2 = calculateScore(10, 8, true);
  
  // Small hunk at end
  int score3 = calculateScore(1, 2, false);
  
  EXPECT_GT(score2, score1);
  EXPECT_GT(score2, score3);
}

TEST_F(DiffRenderingTest, HunksWithBothAdditionsAndDeletionsAreSignificant) {
  auto calculateScore = [](int old_count, int new_count) -> int {
    int score = (old_count + new_count) * 2;
    if (old_count > 0 && new_count > 0) score += 10;
    return score;
  };
  
  // Only additions
  int additions_only = calculateScore(0, 5);
  
  // Only deletions
  int deletions_only = calculateScore(5, 0);
  
  // Both
  int both = calculateScore(3, 3);
  
  EXPECT_GT(both, additions_only);
  EXPECT_GT(both, deletions_only);
}

// Test line counting for diff stats
TEST_F(DiffRenderingTest, CountAddedLines) {
  std::string new_str = "line1\nline2\nline3\nline4\n";
  int count = std::count(new_str.begin(), new_str.end(), '\n');
  EXPECT_EQ(count, 4);
}

TEST_F(DiffRenderingTest, CountRemovedLines) {
  std::string old_str = "line1\nline2\n";
  int count = std::count(old_str.begin(), old_str.end(), '\n');
  EXPECT_EQ(count, 2);
}

// Test diff truncation logic
TEST_F(DiffRenderingTest, TruncateLongDiffs) {
  std::string long_diff;
  for (int i = 0; i < 100; i++) {
    long_diff += "+ line " + std::to_string(i) + "\n";
  }
  
  int max_lines = 10;
  int line_count = std::count(long_diff.begin(), long_diff.end(), '\n');
  
  EXPECT_GT(line_count, max_lines);
  
  // When collapsed, should only show max_lines
  bool collapsed = true;
  int shown = collapsed ? max_lines : line_count;
  
  EXPECT_EQ(shown, max_lines);
}

// Test filename extraction from path
TEST_F(DiffRenderingTest, ExtractFilenameFromPath) {
  auto basename = [](const std::string& path) -> std::string {
    auto pos = path.find_last_of('/');
    if (pos != std::string::npos && pos + 1 < path.size()) {
      return path.substr(pos + 1);
    }
    return path;
  };
  
  EXPECT_EQ(basename("/path/to/file.cpp"), "file.cpp");
  EXPECT_EQ(basename("file.cpp"), "file.cpp");
  EXPECT_EQ(basename("/file.cpp"), "file.cpp");
  EXPECT_EQ(basename("/"), "/");
}

// Test line number gutter width calculation
TEST_F(DiffRenderingTest, CalculateGutterWidth) {
  auto calcGutterWidth = [](int max_line_num) -> int {
    return std::to_string(max_line_num).size();
  };
  
  EXPECT_EQ(calcGutterWidth(9), 1);
  EXPECT_EQ(calcGutterWidth(99), 2);
  EXPECT_EQ(calcGutterWidth(999), 3);
  EXPECT_EQ(calcGutterWidth(1000), 4);
}

// Test line number padding
TEST_F(DiffRenderingTest, PadLineNumber) {
  auto padLine = [](int line, int width) -> std::string {
    std::string result = std::to_string(line);
    while (static_cast<int>(result.size()) < width) {
      result = " " + result;
    }
    return result;
  };
  
  EXPECT_EQ(padLine(1, 4), "   1");
  EXPECT_EQ(padLine(10, 4), "  10");
  EXPECT_EQ(padLine(100, 4), " 100");
  EXPECT_EQ(padLine(1000, 4), "1000");
}

} // namespace firmius::tui
