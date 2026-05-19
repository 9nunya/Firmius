#include "daemon/DaemonLog.hpp"
#include "daemon/DaemonServer.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
std::atomic<bool> g_running{true};
std::atomic<int> g_signalCount{0};
std::atomic<bool> g_shutdownComplete{false};

extern "C" void handleSignal(int signo) {
  g_running.store(false);
  const int count = g_signalCount.fetch_add(1) + 1;
  if (count >= 2) {
    std::_Exit(128 + signo);
  }
}

#if defined(_WIN32)
static BOOL WINAPI consoleCtrlHandler(DWORD dwCtrlType) {
  switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
      handleSignal(SIGINT);
      return TRUE;
    default:
      return FALSE;
  }
}
#endif

void installSignalHandlers() {
#if defined(_WIN32)
  SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = handleSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
#endif
}
}

int main(int argc, char **argv) {
  installSignalHandlers();
  FIRMIUS_DLOG_PHASE("firmiusd starting");

  firmius::daemon::DaemonConnectionInfo connection;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--endpoint") {
      if (i + 1 >= argc) {
        std::cerr << "firmiusd missing value for --endpoint\n";
        return 2;
      }
      connection.endpoint = argv[++i];
      continue;
    }
    if (arg == "--help") {
      std::cout << "usage: firmiusd [--endpoint PATH]\n"
                << "  --endpoint PATH    Unix-socket path (or named pipe on Windows).\n\n"
                << "Environment:\n"
                << "  FIRMIUS_DAEMON_VERBOSE   0 (default), 1, or trace\n"
                << "  FIRMIUS_DAEMON_LOG       Path to redirect daemon logs into\n";
      return 0;
    }
    std::cerr << "firmiusd unknown argument: " << arg << "\n";
    return 2;
  }

  FIRMIUS_DLOG_PHASEF("endpoint=%s",
                      connection.endpoint.empty()
                          ? "<default>"
                          : connection.endpoint.c_str());

  firmius::daemon::DaemonServer server(connection);
  if (!server.start()) {
    std::cerr << "firmiusd failed to bind IPC endpoint";
    if (!server.lastError().empty()) {
      std::cerr << ": " << server.lastError();
    }
    std::cerr << "\n";
    FIRMIUS_DLOG_WARNF("failed to bind IPC endpoint: %s",
                       server.lastError().c_str());
    return 1;
  }

  FIRMIUS_DLOG_PHASE("daemon started, entering main loop");

  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  FIRMIUS_DLOG_PHASE("shutdown requested");

  // Shutdown is normally graceful, but stuck provider/agent threads can block
  // joins indefinitely. A watchdog keeps SIGINT/SIGTERM reliable without
  // detaching live threads into object teardown.
  std::thread shutdownWatchdog([] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (!g_shutdownComplete.load()) {
      std::_Exit(128 + SIGTERM);
    }
  });
  shutdownWatchdog.detach();

  server.stop();
  g_shutdownComplete.store(true);
  FIRMIUS_DLOG_PHASE("daemon stopped cleanly");
  return 0;
}
