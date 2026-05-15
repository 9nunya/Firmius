#include "InputBar.hpp"
#include "Terminal.hpp"

namespace firmius::tui2 {

InputBar::InputBar(const AppState& state) : state_(state) {}

int InputBar::height(int /*width*/) const { return 1; }

std::vector<std::string> InputBar::render(int width) const {
  std::string prompt = ansi::fgRgb(120, 140, 220, " > ");
  std::string input = state_.inputBuffer();

  // Truncate input to fit width (prompt takes ~3 visible chars).
  int maxInput = width - 4;
  if (maxInput > 0 && static_cast<int>(input.size()) > maxInput) {
    input = input.substr(input.size() - maxInput);
  }

  std::string line = prompt + input;
  return {ansi::fitToWidth(line, width)};
}

} // namespace firmius::tui2
