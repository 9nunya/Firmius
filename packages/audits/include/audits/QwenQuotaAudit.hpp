#ifndef FIRMIUS_AUDITS_QWENQUOTAAUDIT_HPP
#define FIRMIUS_AUDITS_QWENQUOTAAUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

class QwenQuotaAudit final : public shared::IAudit {
public:
    std::string getId() const override;
    std::string getDescription() const override;
    shared::AuditResult run(const std::vector<std::string>& args) override;
};

}

#endif
