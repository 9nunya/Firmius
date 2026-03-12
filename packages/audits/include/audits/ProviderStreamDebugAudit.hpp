#pragma once

#include "IAudit.hpp"

namespace firmius::audits {

using namespace firmius::shared;

/**
 * @brief Debug audit that logs EVERY chunk from a provider stream to STDOUT.
 *
 * Usage: firmius_audit --audit provider_stream_debug <provider_id> [model_id]
 */
class ProviderStreamDebugAudit : public IAudit {
public:
    std::string getId() const override;
    std::string getDescription() const override;
    AuditResult run(const std::vector<std::string>& args) override;
};

} // namespace firmius::audits
