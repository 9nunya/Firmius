#ifndef FIRMIUS_TUI_HOTKEYS_HPP
#define FIRMIUS_TUI_HOTKEYS_HPP

#include <ftxui/component/event.hpp>
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui {

enum class HotkeyAction {
  OpenHelp,
  OpenCommandPalette,
  ModeCycle,
  PermissionCycle,
  RetryLastRequest,
  VariantCycle,
  TranscriptUndo,
  TranscriptRedo,
  TranscriptUndoToUserBoundary,
  EditUndo,
  EditRedo,
};

struct HotkeyBindingSpec {
  HotkeyAction action;
  const char *config_key;
  const char *default_label;
  const char *category;
  const char *description;
};

struct HotkeyBinding {
  HotkeyAction action;
  std::string label;
};

struct HotkeyConfig {
  std::vector<HotkeyBinding> bindings;
  std::vector<std::string> warnings;
};

struct HotkeyConflict {
  std::string label;
  std::vector<HotkeyAction> actions;
};

constexpr const char *kOpenHelpHotkeyLabel = "F1";
constexpr const char *kOpenCommandPaletteHotkeyLabel = "Ctrl+Shift+P";
constexpr const char *kModeCycleHotkeyLabel = "Ctrl+Y";
constexpr const char *kPermissionCycleHotkeyLabel = "Ctrl+L";
constexpr const char *kRetryLastRequestHotkeyLabel = "Ctrl+R";
constexpr const char *kVariantCycleHotkeyLabel = "Ctrl+K";
constexpr const char *kTranscriptUndoHotkeyLabel = "Ctrl+Z";
constexpr const char *kTranscriptRedoHotkeyLabel = "Ctrl+Shift+Z";
constexpr const char *kTranscriptUndoToUserBoundaryHotkeyLabel =
    "Alt+Z";
constexpr const char *kEditUndoHotkeyLabel = "Ctrl+Alt+Z";
constexpr const char *kEditRedoHotkeyLabel = "Ctrl+Alt+Shift+Z";

const std::vector<HotkeyBindingSpec> &HotkeyBindingSpecs();
std::vector<HotkeyBinding> DefaultHotkeyBindings();
HotkeyConfig LoadHotkeyConfig();
bool SaveDefaultHotkeyConfigIfMissing();
std::vector<HotkeyConflict>
FindHotkeyConflicts(const std::vector<HotkeyBinding> &bindings);
bool SaveHotkeyConfig(const std::vector<HotkeyBinding> &bindings,
                      std::vector<std::string> *warnings = nullptr);
bool ReloadHotkeyConfig(std::vector<std::string> *warnings = nullptr);
std::vector<HotkeyBinding> ResetHotkeyBindingsToDefaults(
    std::vector<std::string> *warnings = nullptr);
std::optional<std::string> ParseHotkeyLabel(std::string label);
const HotkeyBinding *FindHotkeyBinding(HotkeyAction action);
std::string GetHotkeyLabel(HotkeyAction action);
std::string HotkeyActionName(HotkeyAction action);
std::string HotkeyActionDescription(HotkeyAction action);
bool IsHotkeyActionEvent(HotkeyAction action, const ftxui::Event &event);
bool IsHotkeyActionInput(HotkeyAction action, const std::string &raw);

bool IsOpenHelpEvent(const ftxui::Event &event);
bool IsOpenHelpInput(const std::string &raw);
bool IsOpenCommandPaletteEvent(const ftxui::Event &event);
bool IsOpenCommandPaletteInput(const std::string &raw);
bool IsModeCycleEvent(const ftxui::Event &event);
bool IsModeCycleInput(const std::string &raw);
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

bool IsTranscriptUndoToUserBoundaryEvent(const ftxui::Event &event);
bool IsTranscriptUndoToUserBoundaryInput(const std::string &raw);

bool IsEditUndoEvent(const ftxui::Event &event);
bool IsEditUndoInput(const std::string &raw);

bool IsEditRedoEvent(const ftxui::Event &event);
bool IsEditRedoInput(const std::string &raw);

} // namespace firmius::tui

#endif
