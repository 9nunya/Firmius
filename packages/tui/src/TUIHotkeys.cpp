#include "TUIHotkeys.hpp"

namespace firmius::tui {

bool IsPermissionCycleInput(const std::string &raw) {
  if (raw == "\x19" || raw == "\x1b[25;5u" || raw == "\x1b[27;5;121~" ||
      raw == "\x1b[89;5u") {
    return true;
  }

  if (raw.size() > 5 && raw.rfind("\x1b[", 0) == 0 && raw.back() == 'u') {
    auto semi = raw.find(';', 2);
    if (semi != std::string::npos) {
      try {
        int codepoint = std::stoi(raw.substr(2, semi - 2));
        int modifier = std::stoi(raw.substr(semi + 1, raw.size() - semi - 2));
        bool is_y = codepoint == 'y' || codepoint == 'Y';
        bool has_ctrl = (modifier & 4) != 0;
        if (is_y && has_ctrl) {
          return true;
        }
      } catch (...) {
      }
    }
  }

  return false;
}

bool IsPermissionCycleEvent(const ftxui::Event &event) {
  return event == ftxui::Event::Special("\x19") ||
         IsPermissionCycleInput(event.input());
}

} // namespace firmius::tui
