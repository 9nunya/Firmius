#ifndef FIRMIUS_TUI_CONTROLLERS_BACKGROUND_CONTROLLER_HPP
#define FIRMIUS_TUI_CONTROLLERS_BACKGROUND_CONTROLLER_HPP

#include <filesystem>

namespace firmius::tui {

class BackgroundController {
public:
  static BackgroundController& instance();
  void startFileCache(const std::filesystem::path& root);
  void shutdown();

private:
  BackgroundController() = default;
};

} // namespace firmius::tui

#endif
