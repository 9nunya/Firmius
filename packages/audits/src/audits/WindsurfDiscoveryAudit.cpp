#include "audits/WindsurfDiscoveryAudit.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/WindsurfProvider.hpp"

#include <cstdio>
#include <iomanip>
#include <iostream>

namespace firmius::audits {

using firmius::core::EnvLoader;
using firmius::provider::WindsurfProvider;
using firmius::shared::AuditResult;

std::string WindsurfDiscoveryAudit::getId() const {
  return "windsurf_discovery";
}

std::string WindsurfDiscoveryAudit::getDescription() const {
  return "Synchronously discover Windsurf models via Connect-RPC and dump the "
         "resulting cache.";
}

AuditResult WindsurfDiscoveryAudit::run(const std::vector<std::string> &) {
  AuditResult result;
  result.auditId = getId();
  EnvLoader::load(".env.local");
  firmius::core::Engine::instance();

  auto base = firmius::provider::ProviderRegistry::instance().getProvider(
      WindsurfProvider::kProviderId);
  auto provider = std::dynamic_pointer_cast<WindsurfProvider>(base);
  if (!provider) {
    std::cerr << "windsurf provider not registered" << std::endl;
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  auto accounts = provider->getAccounts();
  if (accounts.empty()) {
    std::cerr << "no Windsurf account on file — run `firmius_audit "
                 "--audit oauth_wizard windsurf` first."
              << std::endl;
    result.exitCode = 2;
    result.passed = false;
    return result;
  }

  std::cout << "Discovering models for: " << accounts.front().identifier
            << std::endl;
  auto n = provider->fetchAndMergeModels(accounts.front());
  std::cout << "  -> " << n << " discovered entries" << std::endl;

  auto models = provider->listModels();
  std::cout << std::left << std::setw(38) << "model_uid" << " | "
            << std::setw(34) << "label" << " | " << std::setw(8) << "ctx"
            << " | " << std::setw(7) << "out" << " | " << std::setw(8)
            << "$/1Mi" << " | " << std::setw(8) << "$/1Mo" << " | mod"
            << std::endl;
  std::cout << std::string(120, '-') << std::endl;
  for (const auto &m : models) {
    std::string mods;
    for (const auto &x : m.modalities) {
      if (!mods.empty()) mods += ",";
      mods += x;
    }
    char prInp[16], prOut[16];
    std::snprintf(prInp, sizeof(prInp), "%.2f", m.pricePer1MInput);
    std::snprintf(prOut, sizeof(prOut), "%.2f", m.pricePer1MOutput);
    std::cout << std::left << std::setw(38) << m.id << " | "
              << std::setw(34)
              << (m.id.size() > 34 ? m.id.substr(0, 33) + "…" : m.id) << " | "
              << std::setw(8) << m.contextWindow << " | " << std::setw(7)
              << m.maxOutputTokens << " | " << std::setw(8) << prInp << " | "
              << std::setw(8) << prOut << " | " << mods << std::endl;
  }
  std::cout << std::endl
            << "(" << models.size() << " total models in cache)" << std::endl;

  result.exitCode = 0;
  result.passed = true;
  return result;
}

} // namespace firmius::audits
