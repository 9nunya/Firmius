#include "components/ChatViewportModel.hpp"

#include <algorithm>
#include <memory>

namespace {

int ReadMeasuredRowHeight(const std::vector<ftxui::Component> &rows,
                          const std::vector<int> &row_height_cache, size_t index,
                          int default_estimated_row_height) {
  if (index >= rows.size()) {
    return default_estimated_row_height;
  }
  if (auto measured =
          std::dynamic_pointer_cast<firmius::tui::RowComponent>(rows[index])) {
    const auto &box = measured->box();
    if (box.y_max >= box.y_min) {
      return std::max(1, box.y_max - box.y_min + 1);
    }
  }
  if (auto copyable = std::dynamic_pointer_cast<firmius::tui::CopyableRowComponent>(
          rows[index])) {
    const auto &box = copyable->box();
    if (box.y_max >= box.y_min) {
      return std::max(1, box.y_max - box.y_min + 1);
    }
  }
  return index < row_height_cache.size() ? row_height_cache[index]
                                         : default_estimated_row_height;
}

} // namespace

namespace firmius::tui {

void EnsureChatRowHeightCache(std::vector<int> &row_height_cache,
                              size_t row_count,
                              int default_estimated_row_height) {
  if (row_height_cache.size() != row_count) {
    row_height_cache.assign(row_count, default_estimated_row_height);
  }
}

void RefreshCachedVisibleHeights(
    const std::vector<ftxui::Component> &rows, std::vector<int> &row_height_cache,
    size_t last_visible_start, size_t last_visible_end,
    int default_estimated_row_height) {
  if (row_height_cache.empty() || last_visible_end <= last_visible_start) {
    return;
  }
  const size_t end = std::min(last_visible_end, row_height_cache.size());
  for (size_t i = last_visible_start; i < end; ++i) {
    row_height_cache[i] =
        ReadMeasuredRowHeight(rows, row_height_cache, i,
                              default_estimated_row_height);
  }
}

int GetTotalChatHistoryHeight(const std::vector<int> &row_height_cache) {
  int total = 0;
  for (int h : row_height_cache) {
    total += h;
  }
  return total;
}

ChatViewportWindow ComputeChatViewportWindow(
    size_t row_count, const std::vector<int> &row_height_cache,
    int viewport_height, int scroll_offset, bool is_at_bottom,
    size_t last_visible_start, size_t last_visible_end,
    int virtualization_overscan_lines) {
  ChatViewportWindow window;
  window.start = 0;
  window.end = row_count;
  if (viewport_height <= 0) {
    return window;
  }

  const bool has_measured_window =
      last_visible_end > last_visible_start &&
      last_visible_end <= row_height_cache.size();
  const bool should_virtualize =
      has_measured_window && row_count > static_cast<size_t>(viewport_height * 3);
  if (!should_virtualize) {
    return window;
  }

  const int history_height = GetTotalChatHistoryHeight(row_height_cache);
  const int anchor_offset = is_at_bottom
                                ? std::max(0, history_height - viewport_height)
                                : scroll_offset;
  const int target_top =
      std::max(0, anchor_offset - virtualization_overscan_lines);
  const int target_bottom =
      anchor_offset + viewport_height + virtualization_overscan_lines;

  int cumulative = 0;
  window.start = row_count;
  window.end = row_count;
  for (size_t i = 0; i < row_height_cache.size(); ++i) {
    const int next = cumulative + row_height_cache[i];
    if (window.start == row_count && next > target_top) {
      window.start = i;
      window.top_padding = cumulative;
    }
    if (next >= target_bottom) {
      window.end = i + 1;
      cumulative = next;
      break;
    }
    cumulative = next;
  }

  if (window.start == row_count) {
    window.start = 0;
    window.top_padding = 0;
  }
  if (window.end < window.start) {
    window.start = 0;
    window.end = row_count;
    window.top_padding = 0;
    window.bottom_padding = 0;
    return window;
  }

  for (size_t i = window.end; i < row_height_cache.size(); ++i) {
    window.bottom_padding += row_height_cache[i];
  }
  return window;
}

} // namespace firmius::tui
