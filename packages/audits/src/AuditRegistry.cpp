#include "AuditRegistry.hpp"
#include "audits/AntigravityProviderAudit.hpp"
#include "audits/LoopCancellationAudit.hpp"
#include "audits/LspAudit.hpp"
#include "audits/ModelLoadAudit.hpp"
#include "audits/OpenRouterQuotaAudit.hpp"
#include "audits/AntigravityQuotaAudit.hpp"
#include "audits/BenchmarksAudit.hpp"
#include "audits/CodexProviderAudit.hpp"
#include "audits/CodexQuotaAudit.hpp"
#include "audits/ContextBudgetAudit.hpp"
#include "audits/EditToolAudit.hpp"
#include "audits/EditHistoryAudit.hpp"
#include "audits/DaemonRuntimeIsolationAudit.hpp"
#include "audits/HarnessChaosAudit.hpp"
#include "audits/OAuthWizardAudit.hpp"
#include "audits/P15Audit.hpp"
#include "audits/PersistenceStressAudit.hpp"
#include "audits/PromisesAudit.hpp"
#include "audits/ProviderAudit.hpp"
#include "audits/ProviderStreamDebugAudit.hpp"
#include "audits/QwenProviderAudit.hpp"
#include "audits/QwenQuotaAudit.hpp"
#include "audits/SubagentStressAudit.hpp"
#include "audits/WorkflowsAudit.hpp"
#include "audits/ResumeTodoAudit.hpp"
#include "audits/WebFetchAudit.hpp"
#include "audits/WebSearchAudit.hpp"
#include "audits/ReasoningTraceAudit.hpp"
#include "audits/TokenWasteAudit.hpp"
#include "audits/CacheAudit.hpp"
#include "audits/EmbeddingAudit.hpp"
#include "audits/WorkingMemoryAudit.hpp"
#include "audits/WorkingMemoryLiveAudit.hpp"

namespace firmius::audits {

std::vector<std::unique_ptr<shared::IAudit>> createAudits() {
    std::vector<std::unique_ptr<shared::IAudit>> audits;
    audits.push_back(std::make_unique<BenchmarksAudit>());
    audits.push_back(std::make_unique<ProviderAudit>());
    audits.push_back(std::make_unique<P15Audit>());
    audits.push_back(std::make_unique<PersistenceStressAudit>());
    audits.push_back(std::make_unique<SubagentStressAudit>());
    audits.push_back(std::make_unique<AntigravityProviderAudit>());
    audits.push_back(std::make_unique<AntigravityQuotaAudit>());
    audits.push_back(std::make_unique<CodexProviderAudit>());
    audits.push_back(std::make_unique<ContextBudgetAudit>());
    audits.push_back(std::make_unique<EditToolAudit>());
    audits.push_back(std::make_unique<EditHistoryAudit>());
    audits.push_back(std::make_unique<DaemonRuntimeIsolationAudit>());
    audits.push_back(std::make_unique<CodexQuotaAudit>());
    audits.push_back(std::make_unique<OpenRouterQuotaAudit>());
    audits.push_back(std::make_unique<QwenProviderAudit>());
    audits.push_back(std::make_unique<QwenQuotaAudit>());
    audits.push_back(std::make_unique<HarnessChaosAudit>());
    audits.push_back(std::make_unique<OAuthWizardAudit>());
    audits.push_back(std::make_unique<PromisesAudit>());
    audits.push_back(std::make_unique<WorkflowsAudit>());
    audits.push_back(std::make_unique<ProviderStreamDebugAudit>());
    audits.push_back(std::make_unique<LoopCancellationAudit>());
    audits.push_back(std::make_unique<LspAudit>());
    audits.push_back(std::make_unique<ModelLoadAudit>());
    audits.push_back(std::make_unique<ResumeTodoAudit>());
    audits.push_back(std::make_unique<WebFetchAudit>());
    audits.push_back(std::make_unique<WebSearchAudit>());
    audits.push_back(std::make_unique<ReasoningTraceAudit>());
    audits.push_back(std::make_unique<TokenWasteAudit>());
    audits.push_back(std::make_unique<CacheAudit>());
    audits.push_back(std::make_unique<EmbeddingAudit>());
    audits.push_back(std::make_unique<WorkingMemoryAudit>());
    audits.push_back(std::make_unique<WorkingMemoryLiveAudit>());
    return audits;

}
}

