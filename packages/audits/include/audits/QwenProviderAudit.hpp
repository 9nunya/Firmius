#ifndef FIRMIUS_AUDITS_QWEN_PROVIDER_AUDIT_HPP
#define FIRMIUS_AUDITS_QWEN_PROVIDER_AUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

class QwenProviderAudit final : public shared::IAudit {
public:
    std::string getId() const override;
    std::string getDescription() const override;
    shared::AuditResult run(const std::vector<std::string>& args) override;
};

}

#endif
