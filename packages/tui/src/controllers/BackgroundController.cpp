#include "controllers/BackgroundController.hpp"

namespace firmius::tui {

BackgroundController& BackgroundController::instance() {
  static BackgroundController inst;
  return inst;
}

void BackgroundController::startFileCache(const std::filesystem::path& root) {
  (void)root;
}

void BackgroundController::shutdown() {
}

} // namespace firmius::tui
