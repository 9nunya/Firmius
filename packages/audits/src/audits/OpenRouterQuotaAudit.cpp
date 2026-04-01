#include "audits/OpenRouterQuotaAudit.hpp"

#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

std::string OpenRouterQuotaAudit::getId() const { return "openrouter_quota"; }

std::string OpenRouterQuotaAudit::getDescription() const {
  return "Show OpenRouter quota buckets for configured API keys";
}

shared::AuditResult
OpenRouterQuotaAudit::run(const std::vector<std::string> &) {
  AuditResult result;
  result.auditId = getId();

  std::cout << "Starting OpenRouter Quota Audit..." << std::endl;
  Panic::init();
  EnvLoader::load(".env.local");

  auto &harness = Harness::instance();
  harness.init();

  std::cout << "\nFetching quotas for 'openrouter' provider..." << std::endl;
  auto quotas = harness.getAllQuotas("openrouter");

  std::cout << "\n--- Quota Display ---\n" << std::endl;
  if (quotas.empty()) {
    std::cout << "No quotas found (or no API keys configured)." << std::endl;
  } else {
    for (const auto &[account, buckets] : quotas) {
      std::cout << "Account: " << account << std::endl;
      if (buckets.empty()) {
        std::cout << "  No quota data available." << std::endl;
        continue;
      }
      for (const auto &bucket : buckets) {
        constexpr int kBarWidth = 20;
        const int filled =
            static_cast<int>(bucket.remainingFraction * kBarWidth);
        std::cout << "  [" << std::left << std::setw(15) << bucket.name
                  << "] [";
        for (int i = 0; i < kBarWidth; ++i) {
          std::cout << (i < filled ? "#" : "-");
        }
        std::cout << "] " << std::fixed << std::setprecision(1)
                  << (bucket.remainingFraction * 100.0f) << "%";
        if (!bucket.resetTime.empty()) {
          std::cout << " (Info: " << bucket.resetTime << ")";
        }
        std::cout << std::endl;
      }
    }
  }
  std::cout << "\n---------------------\n" << std::endl;

  harness.shutdown();
  result.exitCode = 0;
  result.passed = true;
  return result;
}

} // namespace firmius::audits
