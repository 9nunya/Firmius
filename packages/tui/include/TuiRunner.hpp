#include <atomic>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

namespace {

std::atomic<int> g_pending_sigint{0};

extern "C" void HandleSigint(int);

} // namespace

namespace firmius::tui {

void runTui(bool debugging_mode, bool continue_last);

} // namespace firmius::tui
