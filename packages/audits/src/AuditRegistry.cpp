#include "AuditRegistry.hpp"
#include "audits/AntigravityProviderAudit.hpp"
#include "audits/AntigravityQuotaAudit.hpp"
#include "audits/BenchmarksAudit.hpp"
#include "audits/CodexProviderAudit.hpp"
#include "audits/CodexQuotaAudit.hpp"
#include "audits/HarnessChaosAudit.hpp"
#include "audits/P15Audit.hpp"
#include "audits/ProviderAudit.hpp"
#include "audits/QwenProviderAudit.hpp"
#include "audits/QwenQuotaAudit.hpp"
#include "audits/SubagentStressAudit.hpp"

namespace firmius::audits {

std::vector<std::unique_ptr<shared::IAudit>> createAudits() {
    std::vector<std::unique_ptr<shared::IAudit>> audits;
    audits.push_back(std::make_unique<BenchmarksAudit>());
    audits.push_back(std::make_unique<ProviderAudit>());
    audits.push_back(std::make_unique<P15Audit>());
    audits.push_back(std::make_unique<SubagentStressAudit>());
    audits.push_back(std::make_unique<AntigravityProviderAudit>());
    audits.push_back(std::make_unique<AntigravityQuotaAudit>());
    audits.push_back(std::make_unique<CodexProviderAudit>());
    audits.push_back(std::make_unique<CodexQuotaAudit>());
    audits.push_back(std::make_unique<QwenProviderAudit>());
    audits.push_back(std::make_unique<QwenQuotaAudit>());
    audits.push_back(std::make_unique<HarnessChaosAudit>());
    return audits;
}

}
