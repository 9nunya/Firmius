#include "TUIHotkeys.hpp"

#include <vector>

namespace firmius::tui {

namespace {
bool isCtrlLetterInput(const std::string &raw, char lower, char upper,
                       const std::vector<std::string> &knownSequences) {
  for (const auto &sequence : knownSequences) {
    if (raw == sequence) {
      return true;
    }
  }

  if (raw.size() > 5 && raw.rfind("\x1b[", 0) == 0 && raw.back() == 'u') {
    auto semi = raw.find(';', 2);
    if (semi != std::string::npos) {
      try {
        int codepoint = std::stoi(raw.substr(2, semi - 2));
        int modifier = std::stoi(raw.substr(semi + 1, raw.size() - semi - 2));
        bool is_match = codepoint == lower || codepoint == upper;
        bool has_ctrl = (modifier & 4) != 0;
        if (is_match && has_ctrl) {
          return true;
        }
      } catch (...) {
      }
    }
  }

  return false;
}
} // namespace

bool IsPermissionCycleInput(const std::string &raw) {
  return isCtrlLetterInput(
      raw, 'y', 'Y',
      {"\x19", "\x1b[25;5u", "\x1b[27;5;121~", "\x1b[89;5u"});
}

bool IsPermissionCycleEvent(const ftxui::Event &event) {
  return event == ftxui::Event::Special("\x19") ||
         IsPermissionCycleInput(event.input());
}

bool IsRetryLastRequestInput(const std::string &raw) {
  return isCtrlLetterInput(
      raw, 'r', 'R',
      {"\x12", "\x1b[18;5u", "\x1b[27;5;114~", "\x1b[82;5u"});
}

bool IsRetryLastRequestEvent(const ftxui::Event &event) {
  return event == ftxui::Event::Special("\x12") ||
         IsRetryLastRequestInput(event.input());
}

} // namespace firmius::tui
