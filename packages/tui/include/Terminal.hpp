#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace firmius::tui {

/// Full-screen terminal renderer using alternate screen buffer.
///
/// Renders into an alternate screen buffer (like vim/htop). The entire
/// W×H grid is managed by the application — no native terminal scrolling.
/// Custom scrollback is handled at the application layer.
///
/// Supports SGR mouse mode for scroll wheel and click events.
class Terminal {
public:
  Terminal();
  ~Terminal();

  /// Enter alternate screen buffer + raw mode + mouse tracking.
  bool enter();

  /// Leave alternate screen + disable mouse + restore terminal state.
  void leave();

  /// Get terminal dimensions.
  std::pair<int, int> size() const; ///< {width, height}

  bool isActive() const { return active_; }

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
  void clearToEndOfScreen();
  void clearScreen();
  void flush();

  // ── Input ──

  /// Read input from stdin (non-blocking with timeout in ms).
  /// Returns the raw key string. May contain multi-byte escape sequences.
  std::string readKey(int timeoutMs = 50);

  /// Read input with mouse event parsing.
  /// Returns {rawKey, mouseEvent}. If the input was a mouse sequence,
  /// mouseEvent is populated and rawKey is empty.
  std::pair<std::string, std::optional<struct MouseEvent>>
  readInput(int timeoutMs = 50);

  /// Check if terminal was resized since last check.
  bool wasResized();

  /// Write raw ANSI to the batch buffer (or stdout if not batching).
  void rawWrite(const std::string& s);

private:
  std::optional<struct MouseEvent> parseMouseSequence(const std::string& raw);

  bool active_ = false;
  int termWidth_ = 80;
  int termHeight_ = 24;
  std::string inputBuf_;  ///< Buffered partial escape sequences between reads.
  std::chrono::steady_clock::time_point escBufferedAt_{};  ///< When a lone ESC was first buffered.

  /// Output batch buffer. When batching, writes go here instead of stdout.
  std::string batchBuffer_;
  bool batching_ = false;

  struct TermState;
  TermState* savedState_ = nullptr;
};

/// Mouse event parsed from SGR 1006 escape sequences.
struct MouseEvent {
  enum class Type { Press, Release, Scroll, Move };
  enum class Button { Left, Middle, Right, ScrollUp, ScrollDown, None };

  Type type = Type::Press;
  Button button = Button::Left;
  int col = 0;    ///< 1-indexed column
  int row = 0;    ///< 1-indexed row
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
std::string strip(const std::string& text);

/// Visible character width of a string (strips ANSI codes).
int visibleWidth(const std::string& text);

/// Pad/truncate a string to exactly `width` visible characters.
std::string fitToWidth(const std::string& text, int width, char pad = ' ');

} // namespace ansi

} // namespace firmius::tui
