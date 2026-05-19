#include "KeybindRegistry.hpp"

namespace firmius::tui {

void KeybindRegistry::registerKeybind(Keybind keybind) {
  keybinds_.push_back(std::move(keybind));
}

bool KeybindRegistry::handleKey(const std::string &rawKey,
                                 ActivityContext currentContext) {
  for (const auto &kb : keybinds_) {
    if (kb.key != rawKey) continue;
    if (kb.alwaysActive || kb.context == currentContext) {
      if (kb.handler) {
        kb.handler();
      }
      return true;
    }
  }
  return false;
}

std::vector<Keybind> KeybindRegistry::listKeybinds(
    ActivityContext context) const {
  std::vector<Keybind> result;
  for (const auto &kb : keybinds_) {
    if (kb.alwaysActive || kb.context == context) {
      result.push_back(kb);
    }
  }
  return result;
}

std::vector<Keybind> KeybindRegistry::allKeybinds() const {
  return keybinds_;
}

} // namespace firmius::tui
