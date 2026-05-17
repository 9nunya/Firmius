#include "daemon/DaemonServer.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

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

void installSignalHandlers() {
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = handleSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}
}

int main(int argc, char **argv) {
  installSignalHandlers();

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
      std::cout << "usage: firmiusd [--endpoint PATH]\n";
      return 0;
    }
    std::cerr << "firmiusd unknown argument: " << arg << "\n";
    return 2;
  }

  firmius::daemon::DaemonServer server(connection);
  if (!server.start()) {
    std::cerr << "firmiusd failed to bind IPC endpoint";
    if (!server.lastError().empty()) {
      std::cerr << ": " << server.lastError();
    }
    std::cerr << "\n";
    return 1;
  }

  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

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
  return 0;
}
