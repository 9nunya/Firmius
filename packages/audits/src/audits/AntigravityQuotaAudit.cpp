#include "audits/AntigravityQuotaAudit.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

std::string AntigravityQuotaAudit::getId() const { return "antigravity_quota"; }

std::string AntigravityQuotaAudit::getDescription() const { return "Show antigravity quota buckets"; }

shared::AuditResult AntigravityQuotaAudit::run(const std::vector<std::string>&) {
    AuditResult result;
    result.auditId = getId();
    std::cout << "Starting Antigravity Quota Audit..." << std::endl;
    Panic::init();
    EnvLoader::load(".env.local");
    auto& harness = Harness::instance();
    harness.init();
    std::cout << "\nFetching quotas for 'antigravity' provider..." << std::endl;
    auto quotas = harness.getAllQuotas("antigravity");
    std::cout << "\n--- Quota Display ---\n" << std::endl;
    if (quotas.empty()) {
        std::cout << "No quotas found (or no OAuth accounts connected)." << std::endl;
    } else {
        for (const auto& [account, buckets] : quotas) {
            std::cout << "Account: " << account << std::endl;
            if (buckets.empty()) {
                std::cout << "  No quota data available." << std::endl;
                continue;
            }
            for (const auto& bucket : buckets) {
                int barWidth = 20;
                int filled = static_cast<int>(bucket.remainingFraction * barWidth);
                std::cout << "  [" << std::left << std::setw(15) << bucket.name << "] ";
                std::cout << "[";
                for (int i = 0; i < barWidth; ++i) {
                    if (i < filled) std::cout << "#";
                    else std::cout << "-";
                }
                std::cout << "] " << std::fixed << std::setprecision(1) << (bucket.remainingFraction * 100.0f) << "%";
                if (!bucket.resetTime.empty()) {
                    std::cout << " (Resets: " << bucket.resetTime << ")";
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

}
