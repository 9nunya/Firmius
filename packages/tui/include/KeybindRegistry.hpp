#ifndef FIRMIUS_TUI_KEYBINDREGISTRY_HPP
#define FIRMIUS_TUI_KEYBINDREGISTRY_HPP

#include "AppState.hpp"

#include <functional>
#include <string>
#include <vector>

namespace firmius::tui {

/// A registered keybind entry.
struct Keybind {
  std::string key;                    ///< Display string (e.g., "Ctrl+Q").
  std::string description;           ///< Human-readable action name.
  ActivityContext context;            ///< When this keybind is active.
  bool alwaysActive = false;         ///< Overrides context check.
  std::function<void()> handler;     ///< Action to invoke.
};

/// Modular keybind registration and dispatch.
class KeybindRegistry {
public:
  KeybindRegistry() = default;

  /// Register a keybind.
  void registerKeybind(Keybind keybind);

  /// Dispatch a raw key string against registered keybinds.
  /// Returns true if a keybind was matched and invoked.
  bool handleKey(const std::string &rawKey, ActivityContext currentContext);

  /// List keybinds active in the given context (for BottomBar rendering).
  std::vector<Keybind> listKeybinds(ActivityContext context) const;

  /// List all registered keybinds.
  std::vector<Keybind> allKeybinds() const;

private:
  std::vector<Keybind> keybinds_;
};

// ── Key constants ──
namespace keys {

inline constexpr const char *kEnter = "\r";
inline constexpr const char *kEscape = "\x1b";
inline constexpr const char *kCtrlC = "\x03";
inline constexpr const char *kCtrlQ = "\x11";
inline constexpr const char *kCtrlN = "\x0e";
inline constexpr const char *kCtrlB = "\x02";
inline constexpr const char *kCtrlP = "\x10";
inline constexpr const char *kCtrlE = "\x05";
inline constexpr const char *kCtrlT = "\x14";
inline constexpr const char *kCtrlY = "\x19";
inline constexpr const char *kBackspace = "\x7f";
inline constexpr const char *kBackspaceDel = "\b";
inline constexpr const char *kUp = "\x1b[A";
inline constexpr const char *kDown = "\x1b[B";
inline constexpr const char *kPageUp = "\x1b[5~";
inline constexpr const char *kPageDown = "\x1b[6~";
inline constexpr const char *kHome = "\x1b[H";
inline constexpr const char *kEnd = "\x1b[F";

} // namespace keys

} // namespace firmius::tui

#endif // FIRMIUS_TUI_KEYBINDREGISTRY_HPP
