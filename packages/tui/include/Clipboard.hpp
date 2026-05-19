#ifndef FIRMIUS_TUI_CLIPBOARD_HPP
#define FIRMIUS_TUI_CLIPBOARD_HPP

#include <optional>
#include <string>

namespace firmius::tui {

/// Best-effort clipboard. We use OSC 52 by default for setText — that's
/// a terminal escape sequence that any modern terminal (kitty, alacritty,
/// wezterm, foot, iterm2, recent gnome-terminal, ghostty, tmux) honors
/// and writes to the user's system clipboard. It also works over SSH
/// because the sequence is sent to the local terminal, not the remote
/// machine.
///
/// getImage uses subprocess calls to platform-native tools because OSC
/// 52 has no read primitive. Linux probes wl-paste / xclip / xsel,
/// macOS uses pbpaste -Prefer image, Windows uses PowerShell with
/// Get-Clipboard (we keep the Windows path narrow because v2's primary
/// platform is Linux).
class Clipboard {
public:
  /// Send `text` to the system clipboard via OSC 52. Returns true if the
  /// escape sequence was written (which doesn't guarantee the terminal
  /// honored it — that depends on terminal config).
  static bool setText(const std::string& text);

  /// Read an image from the system clipboard. On success, returns the
  /// base64-encoded bytes (NO data:URI prefix) and writes the detected
  /// MIME type into `mimeTypeOut`. Returns nullopt when nothing is
  /// available, when the clipboard contains no image, or when the
  /// platform clipboard tool isn't installed.
  static std::optional<std::string> getImage(std::string& mimeTypeOut);
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_CLIPBOARD_HPP
