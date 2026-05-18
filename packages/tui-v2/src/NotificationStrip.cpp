#include "NotificationStrip.hpp"

#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>
#include <chrono>

namespace firmius::tui2 {

namespace {

// Lerp an RGB color toward the background by `1 - opacity`. opacity=1.0
// returns the original color; opacity=0.0 returns the bg. Terminals
// can't do real alpha, so we simulate a fade by approaching the bg.
ThemeRgb lerpToBg(const ThemeRgb& fg, const ThemeRgb& bg, float opacity) {
  const float a = std::clamp(opacity, 0.0f, 1.0f);
  auto mix = [&](int f, int b) {
    return static_cast<int>(static_cast<float>(b) +
                             (static_cast<float>(f - b) * a));
  };
  return ThemeRgb{mix(fg.r, bg.r), mix(fg.g, bg.g), mix(fg.b, bg.b)};
}

const char* glyphFor(NotificationKind kind) {
  switch (kind) {
  case NotificationKind::Info:    return "•";
  case NotificationKind::Success: return "✓";
  case NotificationKind::Warning: return "!";
  case NotificationKind::Error:   return "✕";
  }
  return "•";
}

// Pick the accent foreground for a notification kind. We deliberately
// use the same `statusBar.context.{low,medium,high}` triple that the
// rest of the TUI uses for "good / caution / bad", and `base.fg` for
// neutral info — keeps theme overrides in one place.
ThemeRgb accentFor(NotificationKind kind, const ThemeSpec& theme) {
  switch (kind) {
  case NotificationKind::Info:    return theme.base.highlight;
  case NotificationKind::Success: return theme.statusBar.context.low;
  case NotificationKind::Warning: return theme.statusBar.context.medium;
  case NotificationKind::Error:   return theme.statusBar.error.normal.fg;
  }
  return theme.base.fg;
}

}  // namespace

NotificationStrip::NotificationStrip(const NotificationCenter& center)
    : center_(center) {}

std::vector<Notification> NotificationStrip::activeStack() const {
  const auto now = std::chrono::steady_clock::now();
  auto all = center_.snapshot();
  // Filter to live (not yet expired) and order oldest→newest so the
  // newest sits at the bottom. We then trim the OLDEST (top) when over
  // the cap — the newest toast is always visible.
  std::vector<Notification> live;
  live.reserve(all.size());
  for (auto &n : all) {
    if (now < n.fadeOutUntil) live.push_back(std::move(n));
  }
  if (static_cast<int>(live.size()) > kMaxVisible) {
    live.erase(live.begin(),
               live.begin() +
                   (static_cast<int>(live.size()) - kMaxVisible));
  }
  return live;
}

int NotificationStrip::height(int /*width*/) const {
  return static_cast<int>(activeStack().size());
}

std::vector<std::string> NotificationStrip::render(int width) const {
  std::vector<std::string> out;
  const auto stack = activeStack();
  if (stack.empty()) return out;

  const auto& theme = ThemeManager::instance().currentTheme();
  const auto now = std::chrono::steady_clock::now();
  const ThemeRgb chatBg = theme.chat.bg;

  for (const auto& n : stack) {
    const auto frame = describeNotification(n, now);
    if (frame.phase == NotificationPhase::Expired) continue;

    const ThemeRgb baseAccent = accentFor(n.kind, theme);
    const ThemeRgb fadedAccent = lerpToBg(baseAccent, chatBg, frame.opacity);
    const ThemeRgb baseText    = theme.base.fg;
    const ThemeRgb fadedText   = lerpToBg(baseText, chatBg, frame.opacity);

    // Body: " <glyph>  <message>"
    std::string glyph = glyphFor(n.kind);
    std::string body  = " " + glyph + "  " + n.message;
    // Right-align the line in a small "card" background. We keep the
    // bg as the chat bg (no panel tint) so the toast feels ambient
    // rather than modal — colour comes from the accent glyph and text.
    std::string colored =
        ansi::fgRgb(fadedAccent.r, fadedAccent.g, fadedAccent.b,
                    " " + glyph + "  ") +
        ansi::fgRgb(fadedText.r, fadedText.g, fadedText.b, n.message);

    out.push_back(ansi::fitToWidth(colored, width));
  }
  return out;
}

}  // namespace firmius::tui2
