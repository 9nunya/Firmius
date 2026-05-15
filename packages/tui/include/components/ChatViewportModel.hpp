#ifndef FIRMIUS_COMPONENTS_CHAT_VIEWPORT_MODEL_HPP
#define FIRMIUS_COMPONENTS_CHAT_VIEWPORT_MODEL_HPP

#include "components/ChatHistoryBuilder.hpp"

#include <ftxui/component/component.hpp>

#include <cstddef>
#include <vector>

namespace firmius::tui {

struct ChatViewportWindow {
  size_t start = 0;
  size_t end = 0;
  int top_padding = 0;
  int bottom_padding = 0;
};

void EnsureChatRowHeightCache(std::vector<int> &row_height_cache,
                              size_t row_count,
                              int default_estimated_row_height);

void RefreshCachedVisibleHeights(
    const std::vector<ftxui::Component> &rows, std::vector<int> &row_height_cache,
    size_t last_visible_start, size_t last_visible_end,
    int default_estimated_row_height);

int GetTotalChatHistoryHeight(const std::vector<int> &row_height_cache);

ChatViewportWindow ComputeChatViewportWindow(
    size_t row_count, const std::vector<int> &row_height_cache,
    int viewport_height, int scroll_offset, bool is_at_bottom,
    size_t last_visible_start, size_t last_visible_end,
    int virtualization_overscan_lines);

} // namespace firmius::tui

#endif
