#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace firmius::tui2 {

/// Progressive terminal renderer.
///
/// Does NOT use alternate screen. Renders inline into the user's existing
/// terminal scrollback. Maintains a line buffer of what's currently on
/// screen and only rewrites lines that changed (diff-based).
///
/// The screen is divided into two zones:
///   1. Scroll zone (top) — content pushed upward via native terminal scroll.
///   2. Pinned zone (bottom) — fixed rows for status/input/menus, rendered
///      via DECSTBM scroll region exclusion.
class Terminal {
public:
  Terminal();
  ~Terminal();

  /// Enter raw mode. Does NOT switch to alternate screen.
  bool enter();

  /// Restore original terminal state.
  void leave();

  /// Get terminal dimensions.
  std::pair<int, int> size() const; ///< {width, height}

  bool isActive() const { return active_; }

  // ── Pinned Zone Management ──

  /// Reserve N rows at the bottom of the terminal for pinned content
  /// (status bar, input, menus). Adjusts the DECSTBM scroll region so
  /// the top zone scrolls independently. Can be called repeatedly as
  /// the pinned area grows/shrinks (e.g. opening a menu).
  void setPinnedHeight(int rows);

  /// Current pinned height.
  int pinnedHeight() const { return pinnedHeight_; }

  /// Row where the pinned zone starts (1-indexed).
  int pinnedTopRow() const;

  // ── Scroll Zone (Transcript) ──

  /// Push a line into the scroll zone. The line appears at the bottom of
  /// the scroll region and everything above shifts up (native terminal
  /// scroll). The line enters the terminal's scrollback buffer.
  void pushLine(const std::string& content);

  /// Push multiple lines at once into the scroll zone.
  void pushLines(const std::vector<std::string>& lines);

  // ── Pinned Zone Rendering ──

  /// Render lines into the pinned zone. The vector maps 1:1 to pinned
  /// rows (index 0 = topmost pinned row). Only rows whose content
  /// differs from the last render are actually written.
  void renderPinned(const std::vector<std::string>& lines);

  // ── Output Buffering ──

  /// Begin a render batch. All writes are buffered until `flushBatch()`.
  void beginBatch();

  /// Flush the buffered output to stdout in one write.
  void flushBatch();

  // ── Cursor ──

  void hideCursor();
  void showCursor();
  void moveCursor(int row, int col);

  // ── Low-level ──

  void clearLine();
  void clearToEndOfLine();
  void clearScreen();
  void flush();

  /// Read a single key press from stdin (non-blocking with timeout in ms).
  std::string readKey(int timeoutMs = 50);

  /// Check if terminal was resized since last check.
  bool wasResized();

private:
  void rawWrite(const std::string& s);
  void setScrollRegion(int top, int bottom);
  void resetScrollRegion();

  bool active_ = false;
  int pinnedHeight_ = 0;
  int termWidth_ = 80;
  int termHeight_ = 24;

  /// What we last rendered into each pinned row. Used for diffing.
  std::vector<std::string> pinnedBuffer_;

  /// Output batch buffer. When batching, writes go here instead of stdout.
  std::string batchBuffer_;
  bool batching_ = false;

  struct TermState;
  TermState* savedState_ = nullptr;
};

// ── ANSI helpers ──
namespace ansi {

std::string bold(const std::string& text);
std::string dim(const std::string& text);
std::string italic(const std::string& text);
std::string underline(const std::string& text);
std::string strikethrough(const std::string& text);
std::string reset();

/// 256-color foreground.
std::string fg(int color, const std::string& text);

/// 256-color background.
std::string bg(int color, const std::string& text);

/// RGB foreground.
std::string fgRgb(int r, int g, int b, const std::string& text);

/// RGB background.
std::string bgRgb(int r, int g, int b, const std::string& text);

/// Invert colors.
std::string invert(const std::string& text);

/// Strip all ANSI escape sequences from a string.
/// Useful for measuring visible width.
std::string strip(const std::string& text);

/// Visible character width of a string (strips ANSI codes).
int visibleWidth(const std::string& text);

/// Pad/truncate a string to exactly `width` visible characters.
std::string fitToWidth(const std::string& text, int width, char pad = ' ');

} // namespace ansi

} // namespace firmius::tui2
