#pragma once

#include "IAudit.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

/**
 * @brief Audit that exercises each known token-waste scenario in the tool layer,
 * running via a live gitlawb/mimo-v2.5-pro agent and measuring actual tool
 * result byte sizes per call.
 *
 * Usage:
 *   firmius_audit --audit token_waste [--provider <id>] [--model <id>]
 *                 [--variant <name>] [--timeout-seconds <n>]
 *
 * Defaults: provider=gitlawb, model=mimo-v2.5-pro
 */
class TokenWasteAudit : public shared::IAudit {
public:
    std::string getId() const override;
    std::string getDescription() const override;
    shared::AuditResult run(const std::vector<std::string>& args) override;
};

} // namespace firmius::audits
