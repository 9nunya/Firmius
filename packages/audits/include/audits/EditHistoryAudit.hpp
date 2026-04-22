#ifndef FIRMIUS_AUDITS_EDITHISTORYAUDIT_HPP
#define FIRMIUS_AUDITS_EDITHISTORYAUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

class EditHistoryAudit final : public shared::IAudit {
public:
    std::string getId() const override;
    std::string getDescription() const override;
    shared::AuditResult run(const std::vector<std::string>& args) override;
};

}

#endif
