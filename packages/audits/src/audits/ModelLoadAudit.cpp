#include "audits/ModelLoadAudit.hpp"

#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "harness/Harness.hpp"
#include "providers/ProviderRegistry.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::provider;
using namespace firmius::shared;

std::string ModelLoadAudit::getId() const { return "model_load"; }

std::string ModelLoadAudit::getDescription() const {
    return "Benchmark TUI model loading via Harness::listAllModels()";
}

shared::AuditResult ModelLoadAudit::run(const std::vector<std::string>&) {
    AuditResult result;
    result.auditId = getId();

    EnvLoader::load(".env.local");
    Engine::instance();

    auto& harness = Harness::instance();
    harness.init();

    const auto configuredProviders = ProviderRegistry::instance().listProviders();
    std::size_t configuredCount = 0;
    for (const auto& provider : configuredProviders) {
        if (provider && provider->isConfigured()) {
            ++configuredCount;
        }
    }

    const auto start = std::chrono::steady_clock::now();

    while (!harness.isModelsLoaded()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto models = harness.listAllModels();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::ostringstream out;
    out << "model_load_ms=" << elapsedMs << "\n";
    out << "models=" << models.size() << "\n";
    out << "configured_providers=" << configuredCount;
    result.output = out.str();
    return result;
}

}
