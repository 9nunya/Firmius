#include "daemon/DaemonServer.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> g_running{true};
extern "C" void handleSignal(int) { g_running.store(false); }
}

int main(int argc, char **argv) {
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

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

  server.stop();
  return 0;
}
