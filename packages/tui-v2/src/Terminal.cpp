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

  // NO alternate screen. We render inline.
  // Hide cursor during rendering.
  rawWrite("\x1b[?25l");

  active_ = true;
  return true;
}

void Terminal::leave() {
  if (!active_) return;

  // Clear the pinned zone so nothing is left behind.
  auto [w, h] = size();
  int startRow = h - pinnedHeight_ + 1;
  for (int i = 0; i < pinnedHeight_; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\x1b[%d;1H", startRow + i);
    rawWrite(buf);
    rawWrite("\x1b[2K");
  }

  // Reset scroll region, show cursor.
  resetScrollRegion();
  rawWrite("\x1b[?25h");

  // Move cursor to the bottom so the shell prompt appears clean.
  char buf[32];
  std::snprintf(buf, sizeof(buf), "\x1b[%d;1H", h);
  rawWrite(buf);
  rawWrite("\n");

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

// ── Pinned Zone Management ──

void Terminal::setPinnedHeight(int rows) {
  auto [w, h] = size();
  termWidth_ = w;
  termHeight_ = h;

  pinnedHeight_ = std::max(0, std::min(rows, h - 2));

  // Set scroll region: top of screen to just above the pinned zone.
  int scrollBottom = h - pinnedHeight_;
  if (scrollBottom < 1) scrollBottom = 1;
  setScrollRegion(1, scrollBottom);

  // Resize the pinned buffer.
  pinnedBuffer_.resize(pinnedHeight_);
}

int Terminal::pinnedTopRow() const {
  return termHeight_ - pinnedHeight_ + 1;
}

// ── Scroll Zone ──

void Terminal::pushLine(const std::string& content) {
  // Position cursor at the bottom of the scroll region.
  int scrollBottom = termHeight_ - pinnedHeight_;
  if (scrollBottom < 1) scrollBottom = 1;

  // Move to last row of scroll region and write a newline to scroll up,
  // then write the content on the new line.
  char buf[32];
  std::snprintf(buf, sizeof(buf), "\x1b[%d;1H", scrollBottom);
  rawWrite(buf);
  rawWrite("\n");
  rawWrite("\x1b[2K"); // Clear the line.
  rawWrite(content);
}

void Terminal::pushLines(const std::vector<std::string>& lines) {
  for (const auto& line : lines) {
    pushLine(line);
  }
}

// ── Pinned Zone Rendering ──

void Terminal::renderPinned(const std::vector<std::string>& lines) {
  int startRow = pinnedTopRow();

  for (int i = 0; i < static_cast<int>(lines.size()) && i < pinnedHeight_; ++i) {
    // Only rewrite if content changed (diff).
    if (i < static_cast<int>(pinnedBuffer_.size()) && pinnedBuffer_[i] == lines[i]) {
      continue;
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "\x1b[%d;1H", startRow + i);
    rawWrite(buf);
    rawWrite("\x1b[2K"); // Clear line.
    rawWrite(lines[i]);
  }

  // Clear any remaining pinned rows that have no content.
  for (int i = static_cast<int>(lines.size()); i < pinnedHeight_; ++i) {
    if (i < static_cast<int>(pinnedBuffer_.size()) && pinnedBuffer_[i].empty()) {
      continue;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\x1b[%d;1H", startRow + i);
    rawWrite(buf);
    rawWrite("\x1b[2K");
  }

  // Update the buffer.
  pinnedBuffer_ = lines;
  pinnedBuffer_.resize(pinnedHeight_);
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
void Terminal::clearScreen() { rawWrite("\x1b[2J\x1b[H"); }
void Terminal::flush() { ::fflush(stdout); }

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

void Terminal::setScrollRegion(int top, int bottom) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "\x1b[%d;%dr", top, bottom);
  rawWrite(buf);
}

void Terminal::resetScrollRegion() { rawWrite("\x1b[r"); }

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
    // Truncate — this is approximate since we can't easily truncate
    // mid-ANSI-sequence. For now return as-is; the terminal will wrap.
    return text;
  }
  return text + std::string(width - visible, pad);
}

} // namespace ansi

} // namespace firmius::tui2
