#include "FatalSignalHandler.hpp"

#include <atomic>
#include <csignal>
#include <initializer_list>

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

namespace firmius::tui {

namespace {

#if !defined(_WIN32)

// All operations here must be async-signal-safe. We avoid stdio, malloc, and
// C++ runtime helpers; only direct write() and signal()/raise().
extern "C" void firmius_fatal_signal_handler(int sig) {
  // Best-effort terminal restore. Sequences:
  //   ESC [?1049l  — leave alternate screen buffer
  //   ESC [?25h    — show cursor
  //   ESC [?2004l  — disable bracketed paste mode
  //   ESC [?1000l ESC [?1002l ESC [?1003l ESC [?1006l — disable mouse tracking
  //   ESC [0m      — reset SGR
  static const char kReset[] =
      "\x1b[?1049l\x1b[?25h\x1b[?2004l"
      "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l"
      "\x1b[0m\r\n";
  // write() is async-signal-safe; ignore short writes / EINTR.
  ssize_t written = ::write(STDERR_FILENO, kReset, sizeof(kReset) - 1);
  (void)written;

  // Reset the disposition and re-raise so the kernel delivers the real
  // crash. We installed with SA_RESETHAND, so disposition is already
  // SIG_DFL by the time this handler runs — but be defensive in case
  // SA_RESETHAND was not honoured.
  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  dfl.sa_flags = 0;
  ::sigaction(sig, &dfl, nullptr);

  // Unblock the signal in case we got here via a path that left it masked.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, sig);
  ::sigprocmask(SIG_UNBLOCK, &mask, nullptr);

  ::raise(sig);
  // If raise() somehow returns, fall through to default abort.
  ::_exit(128 + sig);
}

std::atomic<bool> g_installed{false};

#endif // !_WIN32

} // namespace

void installFatalSignalHandlers() {
#if defined(_WIN32)
  // No-op on Windows: FTXUI's signal swallow loop is a POSIX-specific
  // pathology because Linux re-executes the faulting instruction after a
  // returning SIGSEGV handler. Windows uses SEH for access violations.
  return;
#else
  if (g_installed.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  struct sigaction sa{};
  sa.sa_handler = firmius_fatal_signal_handler;
  sigemptyset(&sa.sa_mask);
  // SA_RESETHAND: after one delivery, restore SIG_DFL automatically. This is
  //   our second line of defence against a re-entered handler.
  // SA_NODEFER:   don't auto-mask the signal during the handler, so a
  //   double-fault produces an immediate default-disposition crash instead
  //   of being held pending.
  sa.sa_flags = SA_RESETHAND | SA_NODEFER;

  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
    ::sigaction(sig, &sa, nullptr);
  }
#endif
}

} // namespace firmius::tui
