#pragma once

#include "IAudit.hpp"

namespace firmius::audits {

// Sends a small "say HELLO" prompt to several Windsurf models discovered live,
// and reports per-model: success/failure, byte count, latency, first chunk
// text (or error). This is the smoke test used to verify the streaming path
// is shippable before we wire it into the TUI.
class WindsurfStreamAudit final : public shared::IAudit {
 public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits
