#ifndef FIRMIUS_AUDITS_CODEXQUOTAAUDIT_HPP
#define FIRMIUS_AUDITS_CODEXQUOTAAUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

class CodexQuotaAudit final : public shared::IAudit {
public:
    std::string getId() const override;
    std::string getDescription() const override;
    shared::AuditResult run(const std::vector<std::string>& args) override;
};

}

#endif
