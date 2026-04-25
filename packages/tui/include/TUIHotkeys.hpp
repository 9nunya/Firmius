#ifndef FIRMIUS_TUI_HOTKEYS_HPP
#define FIRMIUS_TUI_HOTKEYS_HPP

#include <ftxui/component/event.hpp>
#include <string>

namespace firmius::tui {

constexpr const char *kPermissionCycleHotkeyLabel = "Ctrl+Y";
constexpr const char *kRetryLastRequestHotkeyLabel = "Ctrl+R";
constexpr const char *kVariantCycleHotkeyLabel = "Ctrl+K";
constexpr const char *kTranscriptUndoHotkeyLabel = "Ctrl+Z";
constexpr const char *kTranscriptRedoHotkeyLabel = "Ctrl+Shift+Z";
constexpr const char *kEditUndoHotkeyLabel = "Ctrl+Alt+Z";
constexpr const char *kEditRedoHotkeyLabel = "Ctrl+Alt+Shift+Z";

bool IsPermissionCycleEvent(const ftxui::Event &event);
bool IsPermissionCycleInput(const std::string &raw);
bool IsRetryLastRequestEvent(const ftxui::Event &event);
bool IsRetryLastRequestInput(const std::string &raw);
bool IsVariantCycleEvent(const ftxui::Event &event);
bool IsVariantCycleInput(const std::string &raw);
bool IsTranscriptUndoEvent(const ftxui::Event &event);
bool IsTranscriptUndoInput(const std::string &raw);
bool IsTranscriptRedoEvent(const ftxui::Event &event);
bool IsTranscriptRedoInput(const std::string &raw);
bool IsEditUndoEvent(const ftxui::Event &event);
bool IsEditUndoInput(const std::string &raw);
bool IsEditRedoEvent(const ftxui::Event &event);
bool IsEditRedoInput(const std::string &raw);

} // namespace firmius::tui

#endif
