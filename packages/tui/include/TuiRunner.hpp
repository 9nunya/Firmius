#include <atomic>
#include "Enums.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

namespace {

std::atomic<int> g_pending_sigint{0};

extern "C" void HandleSigint(int);

} // namespace

namespace firmius::tui {

struct TuiLaunchOptions {
  bool debuggingMode = false;
  bool continueLast = false;
  bool quitWhenIdle = false;
  firmius::shared::ThreadPermissionMode permissionMode =
      firmius::shared::ThreadPermissionMode::Request;
  std::string initialPrompt;
  std::string initialCwd;
  std::string threadId;
};

void runTui(const TuiLaunchOptions &options);

} // namespace firmius::tui
