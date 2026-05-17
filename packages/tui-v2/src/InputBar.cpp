#include "InputBar.hpp"
#include "Terminal.hpp"
#include "ThemeManager.hpp"

namespace firmius::tui2 {

InputBar::InputBar(const AppState& state) : state_(state) {}

int InputBar::height(int /*width*/) const { return 3; }

int InputBar::cursorRowOffset() const { return 2; }

std::vector<std::string> InputBar::render(int width) const {
  const auto& theme = ThemeManager::instance().currentTheme();
  std::string prompt = ansi::fgRgb(theme.input.prompt.r, theme.input.prompt.g,
                                   theme.input.prompt.b, " ❯ ");
  std::string input = state_.inputBuffer();
  // Build the separator using box-drawing chars to preserve the Claude-style rails.
  std::string separator;
  separator.reserve(std::max(0, width) * 3);
  for (int i = 0; i < width; ++i) {
    separator += "\xE2\x94\x80";
  }

  // Truncate input to fit width (prompt takes ~3 visible chars).
  int maxInput = width - 4;
  if (maxInput > 0 && static_cast<int>(input.size()) > maxInput) {
    input = input.substr(input.size() - maxInput);
  }

  std::string body = input.empty()
                         ? ansi::fgRgb(theme.input.placeholder.r,
                                       theme.input.placeholder.g,
                                       theme.input.placeholder.b,
                                       "type a message...")
                         : ansi::fgRgb(theme.input.fg.r, theme.input.fg.g,
                                       theme.input.fg.b, input);
  std::string line = prompt + body;
  std::string rule = ansi::fgRgb(theme.base.separator.r, theme.base.separator.g,
                                 theme.base.separator.b, separator);
  return {
      ansi::fitToWidth(rule, width),
      ansi::fitToWidth(line, width),
      ansi::fitToWidth(rule, width),
  };
}

} // namespace firmius::tui2
