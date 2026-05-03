#pragma once

#include "IAudit.hpp"

namespace firmius::audits {

// Synchronously runs WindsurfProvider::fetchAndMergeModels() and prints the
// resulting cache (display name, canonical id, ctx, max output, modalities,
// pricing). Exists so we can verify live discovery without going through the
// full ProviderAudit, which also kicks off a streaming chat call that hangs
// against the windsurf streaming endpoint until that surface is implemented.
class WindsurfDiscoveryAudit final : public shared::IAudit {
 public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits
