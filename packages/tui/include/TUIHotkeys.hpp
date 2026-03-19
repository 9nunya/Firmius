#ifndef FIRMIUS_TUI_HOTKEYS_HPP
#define FIRMIUS_TUI_HOTKEYS_HPP

#include <ftxui/component/event.hpp>
#include <string>

namespace firmius::tui {

constexpr const char *kPermissionCycleHotkeyLabel = "Ctrl+Y";
constexpr const char *kRetryLastRequestHotkeyLabel = "Ctrl+R";

bool IsPermissionCycleEvent(const ftxui::Event &event);
bool IsPermissionCycleInput(const std::string &raw);
bool IsRetryLastRequestEvent(const ftxui::Event &event);
bool IsRetryLastRequestInput(const std::string &raw);

} // namespace firmius::tui

#endif
