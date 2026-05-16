#include "Terminal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <unistd.h>
#include <atomic>

#if !defined(_WIN32)
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#endif

namespace firmius::tui2 {

// ── Terminal state ──

struct Terminal::TermState {
#if !defined(_WIN32)
  struct termios original;
#endif
};

namespace {

std::atomic<bool> g_resized{false};

#if !defined(_WIN32)
void sigwinchHandler(int) { g_resized.store(true); }
#endif

} // namespace

Terminal::Terminal() : savedState_(new TermState()) {}

Terminal::~Terminal() {
  if (active_) leave();
  delete savedState_;
}

bool Terminal::enter() {
#if !defined(_WIN32)
  if (::tcgetattr(STDIN_FILENO, &savedState_->original) != 0) return false;

  struct termios raw = savedState_->original;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;

  if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;

  // Install SIGWINCH handler.
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigwinchHandler;
  ::sigaction(SIGWINCH, &sa, nullptr);
#endif

  auto [w, h] = size();
  termWidth_ = w;
  termHeight_ = h;

  // Enter alternate screen buffer.
  rawWrite("\x1b[?1049h");
  // Clear screen and home cursor.
  rawWrite("\x1b[2J\x1b[H");
  // Hide cursor during rendering.
  rawWrite("\x1b[?25l");
  // Enable mouse tracking (SGR 1006 mode).
  rawWrite("\x1b[?1000h"); // Basic mouse tracking
  rawWrite("\x1b[?1002h"); // Button-event tracking
  rawWrite("\x1b[?1003h"); // Any-event tracking (motion)
  rawWrite("\x1b[?1006h"); // SGR mouse mode

  active_ = true;
  return true;
}

void Terminal::leave() {
  if (!active_) return;

  // Disable mouse tracking.
  rawWrite("\x1b[?1006l");
  rawWrite("\x1b[?1003l");
  rawWrite("\x1b[?1002l");
  rawWrite("\x1b[?1000l");
  // Show cursor.
  rawWrite("\x1b[?25h");
  // Leave alternate screen buffer (restores previous screen content).
  rawWrite("\x1b[?1049l");

  flush();

#if !defined(_WIN32)
  ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &savedState_->original);
#endif

  active_ = false;
}

std::pair<int, int> Terminal::size() const {
#if !defined(_WIN32)
  struct winsize ws;
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    return {ws.ws_col, ws.ws_row};
  }
#endif
  return {80, 24};
}

// ── Output Buffering ──

void Terminal::beginBatch() {
  batching_ = true;
  batchBuffer_.clear();
}

void Terminal::flushBatch() {
  if (!batchBuffer_.empty()) {
    (void)::write(STDOUT_FILENO, batchBuffer_.data(), batchBuffer_.size());
  }
  batchBuffer_.clear();
  batching_ = false;
}

// ── Cursor & Low-level ──

void Terminal::hideCursor() { rawWrite("\x1b[?25l"); }
void Terminal::showCursor() { rawWrite("\x1b[?25h"); }

void Terminal::moveCursor(int row, int col) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
  rawWrite(buf);
}

void Terminal::clearLine() { rawWrite("\x1b[2K"); }
void Terminal::clearToEndOfLine() { rawWrite("\x1b[K"); }
void Terminal::clearToEndOfScreen() { rawWrite("\x1b[J"); }
void Terminal::clearScreen() { rawWrite("\x1b[2J\x1b[H"); }
void Terminal::flush() { ::fflush(stdout); }

// ── Input ──

std::string Terminal::readKey(int timeoutMs) {
#if !defined(_WIN32)
  struct pollfd pfd;
  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;

  if (::poll(&pfd, 1, timeoutMs) <= 0) return "";

  char buf[16];
  ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
  if (n <= 0) return "";
  return std::string(buf, static_cast<size_t>(n));
#else
  (void)timeoutMs;
  return "";
#endif
}

std::pair<std::string, std::optional<MouseEvent>>
Terminal::readInput(int timeoutMs) {
  // Prepend any buffered bytes from a previous partial read.
  std::string raw;
  if (!inputBuf_.empty()) {
    raw = std::move(inputBuf_);
    inputBuf_.clear();
  }

  // Read fresh bytes (short timeout if we already have buffered data).
  std::string fresh = readKey(raw.empty() ? timeoutMs : 5);
  raw += fresh;

  if (raw.empty()) return {"", std::nullopt};

  // Try to parse as SGR mouse sequence: ESC [ < button ; col ; row M/m
  if (raw.size() >= 6 && raw[0] == '\x1b' && raw[1] == '[' && raw[2] == '<') {
    // Check if we have the final M/m terminator.
    size_t termPos = std::string::npos;
    for (size_t i = 3; i < raw.size(); ++i) {
      if (raw[i] == 'M' || raw[i] == 'm') {
        termPos = i;
        break;
      }
    }
    if (termPos != std::string::npos) {
      // We have a terminator — try to parse.
      std::string seq = raw.substr(0, termPos + 1);
      auto mouse = parseMouseSequence(seq);
      if (mouse.has_value()) {
        // Buffer any remaining bytes after the mouse sequence.
        if (termPos + 1 < raw.size()) {
          inputBuf_ = raw.substr(termPos + 1);
        }
        escBufferedAt_ = {};
        return {"", mouse};
      }
    }
    // Starts like a mouse sequence but no terminator yet — buffer and wait.
    inputBuf_ = raw;
    return {"", std::nullopt};
  }

  // Starts with ESC but not a recognized sequence prefix — could be a
  // partial arrow/function key. Buffer if it looks incomplete.
  if (raw[0] == '\x1b' && raw.size() == 1) {
    // If we already had a buffered ESC and this is the same one (no new bytes
    // arrived within the timeout), return it as a standalone Escape key.
    auto now = std::chrono::steady_clock::now();
    if (escBufferedAt_ != std::chrono::steady_clock::time_point{} &&
        now - escBufferedAt_ > std::chrono::milliseconds(50)) {
      // Timed out — treat as standalone Escape.
      escBufferedAt_ = {};
      inputBuf_.clear();
      return {raw, std::nullopt};
    }
    // Just ESC — buffer it, more bytes may follow.
    if (escBufferedAt_ == std::chrono::steady_clock::time_point{}) {
      escBufferedAt_ = now;
    }
    inputBuf_ = raw;
    return {"", std::nullopt};
  }

  // Check for partial CSI (ESC [ without a final byte).
  if (raw.size() >= 2 && raw[0] == '\x1b' && raw[1] == '[') {
    // CSI sequences end with a byte in 0x40-0x7E range.
    bool hasFinal = false;
    for (size_t i = 2; i < raw.size(); ++i) {
      uint8_t ch = static_cast<uint8_t>(raw[i]);
      if (ch >= 0x40 && ch <= 0x7E) {
        hasFinal = true;
        break;
      }
    }
    if (!hasFinal) {
      // Incomplete CSI — buffer and wait for more bytes.
      inputBuf_ = raw;
      return {"", std::nullopt};
    }
  }

  // Regular key or complete escape sequence — return as-is.
  escBufferedAt_ = {};
  return {raw, std::nullopt};
}

std::optional<MouseEvent> Terminal::parseMouseSequence(const std::string& raw) {
  // SGR 1006 format: ESC [ < button ; col ; row M (press) or m (release)
  // Minimum: "\x1b[<0;1;1M" = 9 bytes
  if (raw.size() < 6) return std::nullopt;
  if (raw[0] != '\x1b' || raw[1] != '[' || raw[2] != '<') return std::nullopt;

  // Find the final 'M' or 'm'.
  char final = raw.back();
  if (final != 'M' && final != 'm') return std::nullopt;

  // Parse the numbers between '<' and 'M'/'m'.
  std::string params = raw.substr(3, raw.size() - 4);
  int vals[3] = {0, 0, 0};
  int valIdx = 0;
  int current = 0;
  bool hasDigit = false;

  for (char c : params) {
    if (c == ';') {
      if (valIdx < 3) {
        vals[valIdx++] = current;
        current = 0;
        hasDigit = false;
      }
    } else if (c >= '0' && c <= '9') {
      current = current * 10 + (c - '0');
      hasDigit = true;
    } else {
      return std::nullopt; // unexpected character
    }
  }
  if (hasDigit && valIdx < 3) {
    vals[valIdx] = current;
  }

  if (valIdx < 2) return std::nullopt; // need at least button, col, row

  MouseEvent event;
  event.col = vals[1];
  event.row = vals[2];

  int button = vals[0];

  // Decode button bits:
  // bits 0-2: button (0=left, 1=middle, 2=right)
  // bit 5: motion (mouse move while button held, or hover with button 35/43)
  // bit 6: scroll (if set, bits 0-1: 0=up, 1=down)
  if (button & 64) {
    // Scroll event
    event.type = MouseEvent::Type::Scroll;
    event.button = (button & 1) ? MouseEvent::Button::ScrollDown
                                : MouseEvent::Button::ScrollUp;
  } else if ((button & 32) && final == 'M') {
    // Motion event (mouse move).
    event.type = MouseEvent::Type::Move;
    int btn = button & 3;
    if (btn == 0) event.button = MouseEvent::Button::Left;
    else if (btn == 1) event.button = MouseEvent::Button::Middle;
    else if (btn == 2) event.button = MouseEvent::Button::Right;
    else event.button = MouseEvent::Button::None;
  } else {
    event.type = (final == 'M') ? MouseEvent::Type::Press
                                : MouseEvent::Type::Release;
    int btn = button & 3;
    if (btn == 0) event.button = MouseEvent::Button::Left;
    else if (btn == 1) event.button = MouseEvent::Button::Middle;
    else if (btn == 2) event.button = MouseEvent::Button::Right;
    else event.button = MouseEvent::Button::None;
  }

  return event;
}

bool Terminal::wasResized() {
  bool resized = g_resized.exchange(false);
  if (resized) {
    auto [w, h] = size();
    termWidth_ = w;
    termHeight_ = h;
  }
  return resized;
}

void Terminal::rawWrite(const std::string& s) {
  if (batching_) {
    batchBuffer_ += s;
  } else {
    (void)::write(STDOUT_FILENO, s.data(), s.size());
  }
}

// ── ANSI helpers ──

namespace ansi {

std::string bold(const std::string& text) {
  return "\x1b[1m" + text + "\x1b[22m";
}

std::string dim(const std::string& text) {
  return "\x1b[2m" + text + "\x1b[22m";
}

std::string italic(const std::string& text) {
  return "\x1b[3m" + text + "\x1b[23m";
}

std::string underline(const std::string& text) {
  return "\x1b[4m" + text + "\x1b[24m";
}

std::string strikethrough(const std::string& text) {
  return "\x1b[9m" + text + "\x1b[29m";
}

std::string reset() { return "\x1b[0m"; }

std::string fg(int color, const std::string& text) {
  return "\x1b[38;5;" + std::to_string(color) + "m" + text + "\x1b[39m";
}

std::string bg(int color, const std::string& text) {
  return "\x1b[48;5;" + std::to_string(color) + "m" + text + "\x1b[49m";
}

std::string fgRgb(int r, int g, int b, const std::string& text) {
  return "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" +
         std::to_string(b) + "m" + text + "\x1b[39m";
}

std::string bgRgb(int r, int g, int b, const std::string& text) {
  return "\x1b[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" +
         std::to_string(b) + "m" + text + "\x1b[49m";
}

std::string invert(const std::string& text) {
  return "\x1b[7m" + text + "\x1b[27m";
}

std::string strip(const std::string& text) {
  // Remove all ANSI escape sequences: ESC[ ... (letter)
  static const std::regex ansiRegex("\x1b\\[[0-9;]*[A-Za-z]");
  return std::regex_replace(text, ansiRegex, "");
}

int visibleWidth(const std::string& text) {
  return static_cast<int>(strip(text).size());
}

std::string fitToWidth(const std::string& text, int width, char pad) {
  int visible = visibleWidth(text);
  if (visible >= width) {
    return text;
  }
  return text + std::string(width - visible, pad);
}

} // namespace ansi

} // namespace firmius::tui2
