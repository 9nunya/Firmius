#include "harness/Harness.hpp"
#include <EnvLoader.hpp>
#include <Panic.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

using namespace firmius::core;
using namespace firmius::shared;

void printQuotas(
    const std::map<std::string, std::vector<QuotaBucket>> &allQuotas) {
  if (allQuotas.empty()) {
    std::cout << "No quotas found (or no OAuth accounts connected)."
              << std::endl;
    return;
  }

  for (const auto &[account, buckets] : allQuotas) {
    std::cout << "Account: " << account << std::endl;
    if (buckets.empty()) {
      std::cout << "  No quota data available." << std::endl;
      continue;
    }

    for (const auto &bucket : buckets) {
      int barWidth = 20;
      int filled = static_cast<int>(bucket.remainingFraction * barWidth);

      std::cout << "  [" << std::left << std::setw(15) << bucket.name << "] ";
      std::cout << "[";
      for (int i = 0; i < barWidth; ++i) {
        if (i < filled)
          std::cout << "#";
        else
          std::cout << "-";
      }
      std::cout << "] " << std::fixed << std::setprecision(1)
                << (bucket.remainingFraction * 100.0f) << "%";

      if (!bucket.resetTime.empty()) {
        std::cout << " (Resets: " << bucket.resetTime << ")";
      }
      std::cout << std::endl;
    }
  }
}

int main() {
  std::cout << "Starting Antigravity Quota Audit..." << std::endl;
  Panic::init();
  EnvLoader::load(".env.local");

  auto &harness = Harness::instance();
  harness.init();

  std::cout << "\nFetching quotas for 'antigravity' provider..." << std::endl;
  auto quotas = harness.getAllQuotas("antigravity");

  std::cout << "\n--- Quota Display ---\n" << std::endl;
  printQuotas(quotas);
  std::cout << "\n---------------------\n" << std::endl;

  harness.shutdown();
  return 0;
}
