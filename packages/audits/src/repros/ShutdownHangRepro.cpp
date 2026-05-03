#include "EnvLoader.hpp"
#include "Panic.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

// Repro-only: reach into Harness internals to deterministically create a
// joinable background thread that never finishes.
// This stays confined to the repro surface and does not change product code.
#define private public
#include "harness/Harness.hpp"
#undef private

using clock_type = std::chrono::steady_clock;

namespace {


void usage(const char *argv0) {
  std::cerr << "Usage: " << (argv0 ? argv0 : "firmius_shutdown_hang_repro")
            << " [--inject-hang=0|1] [--sleep-before-shutdown-ms=N]\n";
}

int parseInt(const char *s, int fallback) {
  if (!s) return fallback;
  try {
    return std::stoi(s);
  } catch (...) {
    return fallback;
  }
}

} // namespace

int main(int argc, char **argv) {
  int injectHang = 1;
  int sleepBeforeShutdownMs = 50;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return 0;
    }
    if (arg.rfind("--inject-hang=", 0) == 0) {
      injectHang = parseInt(arg.c_str() + std::strlen("--inject-hang="), 1);
      continue;
    }
    if (arg.rfind("--sleep-before-shutdown-ms=", 0) == 0) {
      sleepBeforeShutdownMs =
          parseInt(arg.c_str() + std::strlen("--sleep-before-shutdown-ms="), 50);
      continue;
    }

    std::cerr << "Unknown arg: " << arg << "\n";
    usage(argv[0]);
    return 2;
  }

  firmius::shared::Panic::init();
  firmius::shared::EnvLoader::load(".env.local");

  auto &h = firmius::core::Harness::instance();
  h.init();

  std::cerr << "[repro] injectHang=" << injectHang
            << " sleepBeforeShutdownMs=" << sleepBeforeShutdownMs << "\n";

  if (injectHang) {
    // A never-ending joinable thread placed into Harness::backgroundThreads_.
    // Harness::shutdown will move() backgroundThreads_ and join each thread
    // synchronously (see packages/core/src/Harness.cpp).
    h.backgroundThreads_.emplace_back([]() {
      std::mutex m;
      std::condition_variable cv;
      std::unique_lock<std::mutex> lk(m);
      cv.wait(lk); // wait forever
    });
    std::cerr << "[repro] injected one non-terminating joinable thread\n";
  }

  if (sleepBeforeShutdownMs > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepBeforeShutdownMs));
  }

  const auto t0 = clock_type::now();
  std::cerr << "[repro] calling Harness::shutdown()...\n";
  h.shutdown();
  const auto t1 = clock_type::now();

  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
  std::cerr << "[repro] Harness::shutdown() returned after " << ms.count() << "ms\n";

  return 0;
}
