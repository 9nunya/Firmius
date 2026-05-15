#include "components/ChatCopySelection.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::tui {

int FindCopyableRowAt(
    const std::vector<std::shared_ptr<CopyableRowComponent>> &copyable_rows,
    int x, int y) {
  for (size_t i = 0; i < copyable_rows.size(); ++i) {
    if (!copyable_rows[i]) {
      continue;
    }
    const auto &box = copyable_rows[i]->box();
    if (box.x_min <= x && x <= box.x_max && box.y_min <= y && y <= box.y_max) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool IsPointInCopyableRow(
    const std::vector<std::shared_ptr<CopyableRowComponent>> &copyable_rows,
    int x, int y) {
  return FindCopyableRowAt(copyable_rows, x, y) >= 0;
}

std::string ExtractCopyableTextFromScreen(
    const std::vector<std::shared_ptr<CopyableRowComponent>> &copyable_rows,
    ftxui::ScreenInteractive &screen, int start_x, int start_y, int end_x,
    int end_y) {
  const int x0 = std::clamp(start_x, 0, std::max(0, screen.dimx() - 1));
  const int y0 = std::clamp(start_y, 0, std::max(0, screen.dimy() - 1));
  const int x1 = std::clamp(end_x, 0, std::max(0, screen.dimx() - 1));
  const int y1 = std::clamp(end_y, 0, std::max(0, screen.dimy() - 1));

  const int left = std::min(x0, x1);
  const int right_seed = std::max(x0, x1);
  const int top = std::min(y0, y1);
  const int bottom = std::max(y0, y1);

  std::vector<std::string> lines;
  lines.reserve(static_cast<size_t>(bottom - top + 1));

  for (int y = top; y <= bottom; ++y) {
    int right = right_seed;
    while (right >= left &&
           !IsPointInCopyableRow(copyable_rows, right, y)) {
      --right;
    }
    if (right < left) {
      lines.emplace_back();
      continue;
    }

    std::string line;
    for (int x = left; x <= right; ++x) {
      if (!IsPointInCopyableRow(copyable_rows, x, y)) {
        line.push_back(' ');
        continue;
      }
      const std::string &cell = screen.at(x, y);
      line += cell.empty() ? " " : cell;
    }

    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
  }

  while (!lines.empty() && lines.front().empty()) {
    lines.erase(lines.begin());
  }
  while (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }

  std::ostringstream out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      out << '\n';
    }
    out << lines[i];
  }
  return out.str();
}

void ClearFrameworkSelection(ftxui::ScreenInteractive &screen, int x, int y) {
  ftxui::Mouse pressed;
  pressed.button = ftxui::Mouse::Left;
  pressed.motion = ftxui::Mouse::Pressed;
  pressed.x = x;
  pressed.y = y;

  ftxui::Mouse released = pressed;
  released.motion = ftxui::Mouse::Released;

  screen.PostEvent(ftxui::Event::Mouse("", pressed));
  screen.PostEvent(ftxui::Event::Mouse("", released));
}

} // namespace firmius::tui
