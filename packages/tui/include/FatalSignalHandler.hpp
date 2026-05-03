#ifndef FIRMIUS_TUI_FATAL_SIGNAL_HANDLER_HPP
#define FIRMIUS_TUI_FATAL_SIGNAL_HANDLER_HPP

namespace firmius::tui {

// Override FTXUI's swallow-and-return SIGSEGV/SIGBUS/SIGILL/SIGFPE handler with
// one that:
//   1. Restores the terminal (leave alt screen, show cursor, disable
//      bracketed paste) using only async-signal-safe writes.
//   2. Resets the signal disposition to SIG_DFL and re-raises, so the kernel
//      delivers a real crash + (optional) core dump and the process actually
//      exits instead of spinning on the same faulting instruction forever.
//
// FTXUI installs `RecordSignal` on these signals during ScreenInteractive::Loop
// startup; that handler simply stashes the signal and returns, which on Linux
// causes the kernel to re-execute the faulting instruction and fault again
// indefinitely (the "frozen TUI but PID alive" symptom). Calling this *after*
// the loop has begun overrides FTXUI's handler.
//
// Idempotent. Safe to call multiple times.
void installFatalSignalHandlers();

} // namespace firmius::tui

#endif // FIRMIUS_TUI_FATAL_SIGNAL_HANDLER_HPP
