#pragma once

#include "Terminal.hpp"
#include "ThemeManager.hpp"

#include <string>

namespace firmius::tui::theme_ansi {

inline const ThemeSpec& theme() {
  return ThemeManager::instance().currentTheme();
}

inline std::string fg(const ThemeRgb& color, const std::string& text) {
  return ansi::fgRgb(color.r, color.g, color.b, text);
}

inline std::string bg(const ThemeRgb& color, const std::string& text) {
  return ansi::bgRgb(color.r, color.g, color.b, text);
}

inline std::string accent(const std::string& text) {
  return fg(theme().base.highlight, text);
}

inline std::string foreground(const std::string& text) {
  return fg(theme().base.fg, text);
}

inline std::string dim(const std::string& text) {
  return ansi::dim(fg(theme().base.dim, text));
}

// NOTE: success() and warning() render plain colored text on the chat
// background. They intentionally do NOT use statusBar.streaming.normal.fg /
// statusBar.executingTool.normal.fg because those colors are paired with a
// bright pill bg in the theme schema and are typically near-black — using
// them as fg on the chat bg would produce invisible text in most themes.
// Instead we use the semantic context.low (green/teal — "good") and
// context.medium (amber/yellow — "caution") accents, which every theme
// defines to contrast against base.bg.
inline std::string success(const std::string& text) {
  return fg(theme().statusBar.context.low, text);
}

inline std::string successBg(const std::string& text) {
  return bg(theme().statusBar.streaming.normal.bg,
            fg(theme().statusBar.streaming.normal.fg, text));
}

inline std::string warning(const std::string& text) {
  return fg(theme().statusBar.context.medium, text);
}

inline std::string warningBg(const std::string& text) {
  return bg(theme().statusBar.executingTool.normal.bg,
            fg(theme().statusBar.executingTool.normal.fg, text));
}

inline std::string error(const std::string& text) {
  return fg(theme().statusBar.error.normal.fg, text);
}

inline std::string errorBg(const std::string& text) {
  return bg(theme().statusBar.error.normal.bg,
            fg(theme().statusBar.error.normal.fg, text));
}

inline std::string panel(const std::string& text) {
  return bg(theme().base.separator, text);
}

inline std::string altPanel(const std::string& text) {
  return bg(theme().statusBar.fillerBg, text);
}

inline std::string selection(const std::string& text) {
  // Use reverse video instead of a custom bg. Some themes (jelly, etc.)
  // happen to set agentStrip.pills.contextBg to the same value as the chat
  // bg, which makes a "selection-tinted" row visually identical to an
  // unselected one — bad UX. Reverse video flips fg/bg of whatever the
  // text already had, which guarantees a visible contrast on every theme.
  return ansi::invert(text);
}

inline std::string code(const std::string& text) {
  return bg(theme().base.separator, fg(theme().base.fg, text));
}

inline std::string divider(int width, char ch = '-') {
  return fg(theme().base.separator, std::string(std::max(0, width), ch));
}

} // namespace firmius::tui::theme_ansi
