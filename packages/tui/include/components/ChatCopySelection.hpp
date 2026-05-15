#ifndef FIRMIUS_COMPONENTS_CHAT_COPY_SELECTION_HPP
#define FIRMIUS_COMPONENTS_CHAT_COPY_SELECTION_HPP

#include "components/ChatHistoryBuilder.hpp"

#include <ftxui/component/screen_interactive.hpp>

#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

int FindCopyableRowAt(
    const std::vector<std::shared_ptr<CopyableRowComponent>> &copyable_rows,
    int x, int y);

bool IsPointInCopyableRow(
    const std::vector<std::shared_ptr<CopyableRowComponent>> &copyable_rows,
    int x, int y);

std::string ExtractCopyableTextFromScreen(
    const std::vector<std::shared_ptr<CopyableRowComponent>> &copyable_rows,
    ftxui::ScreenInteractive &screen, int start_x, int start_y, int end_x,
    int end_y);

void ClearFrameworkSelection(ftxui::ScreenInteractive &screen, int x, int y);

} // namespace firmius::tui

#endif
