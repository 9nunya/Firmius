#ifndef FIRMIUS_TUI_FILE_EDIT_DIFF_HPP
#define FIRMIUS_TUI_FILE_EDIT_DIFF_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace firmius::tui {

struct DiffLine {
  char type = ' ';
  int oldLine = 0;
  int newLine = 0;
  std::string content;
};

struct DiffHunk {
  int old_start = 0;
  int old_count = 0;
  int new_start = 0;
  int new_count = 0;
  std::vector<DiffLine> lines;
};

struct DiffStats {
  int added = 0;
  int removed = 0;
};

std::vector<DiffHunk> BuildDiffHunks(const std::string &oldStr,
                                     const std::string &newStr);
std::vector<size_t> RankHunksByRelevance(const std::vector<DiffHunk> &hunks);
DiffStats CountDiffStats(const std::vector<DiffHunk> &hunks);

} // namespace firmius::tui

#endif
