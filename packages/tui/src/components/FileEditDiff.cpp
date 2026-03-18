#include "components/FileEditDiff.hpp"
#include <algorithm>
#include <sstream>

namespace firmius::tui {

namespace {

std::vector<std::string> SplitLines(const std::string &input) {
  std::vector<std::string> lines;
  std::istringstream ss(input);
  std::string line;
  while (std::getline(ss, line)) {
    lines.push_back(line);
  }
  return lines;
}

} // namespace

std::vector<DiffHunk> BuildDiffHunks(const std::string &oldStr,
                                     const std::string &newStr) {
  const auto oldLines = SplitLines(oldStr);
  const auto newLines = SplitLines(newStr);

  const size_t n = oldLines.size();
  const size_t m = newLines.size();
  std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));

  for (size_t i = n; i-- > 0;) {
    for (size_t j = m; j-- > 0;) {
      if (oldLines[i] == newLines[j]) {
        lcs[i][j] = lcs[i + 1][j + 1] + 1;
      } else {
        lcs[i][j] = std::max(lcs[i + 1][j], lcs[i][j + 1]);
      }
    }
  }

  std::vector<DiffLine> diffLines;
  size_t i = 0;
  size_t j = 0;
  int oldLineNo = 1;
  int newLineNo = 1;

  while (i < n && j < m) {
    if (oldLines[i] == newLines[j]) {
      diffLines.push_back({' ', oldLineNo++, newLineNo++, oldLines[i]});
      ++i;
      ++j;
      continue;
    }

    if (lcs[i + 1][j] >= lcs[i][j + 1]) {
      diffLines.push_back({'-', oldLineNo++, 0, oldLines[i]});
      ++i;
    } else {
      diffLines.push_back({'+', 0, newLineNo++, newLines[j]});
      ++j;
    }
  }

  while (i < n) {
    diffLines.push_back({'-', oldLineNo++, 0, oldLines[i++]});
  }
  while (j < m) {
    diffLines.push_back({'+', 0, newLineNo++, newLines[j++]});
  }

  std::vector<DiffHunk> hunks;
  DiffHunk current;
  bool inHunk = false;

  for (const auto &line : diffLines) {
    if (line.type == ' ') {
      if (inHunk) {
        hunks.push_back(current);
        current = DiffHunk{};
        inHunk = false;
      }
      continue;
    }

    if (!inHunk) {
      current.old_start = line.oldLine > 0 ? line.oldLine : line.newLine;
      current.new_start = line.newLine > 0 ? line.newLine : line.oldLine;
      inHunk = true;
    }

    current.lines.push_back(line);
    if (line.oldLine > 0) {
      current.old_count++;
    }
    if (line.newLine > 0) {
      current.new_count++;
    }
  }

  if (inHunk) {
    hunks.push_back(current);
  }

  return hunks;
}

std::vector<size_t> RankHunksByRelevance(const std::vector<DiffHunk> &hunks) {
  std::vector<std::pair<size_t, int>> scored;

  for (size_t i = 0; i < hunks.size(); i++) {
    const auto &hunk = hunks[i];
    int score = (hunk.old_count + hunk.new_count) * 2;
    if (hunk.old_count > 0 && hunk.new_count > 0) {
      score += 10;
    }
    if (hunk.old_start > 1 && i + 1 < hunks.size()) {
      score += 5;
    }
    scored.push_back({i, score});
  }

  std::sort(scored.begin(), scored.end(),
            [](const auto &lhs, const auto &rhs) { return lhs.second > rhs.second; });

  std::vector<size_t> ranked;
  ranked.reserve(scored.size());
  for (const auto &[index, _] : scored) {
    ranked.push_back(index);
  }
  return ranked;
}

DiffStats CountDiffStats(const std::vector<DiffHunk> &hunks) {
  DiffStats stats;
  for (const auto &hunk : hunks) {
    for (const auto &line : hunk.lines) {
      if (line.type == '+') {
        stats.added++;
      } else if (line.type == '-') {
        stats.removed++;
      }
    }
  }
  return stats;
}

} // namespace firmius::tui
