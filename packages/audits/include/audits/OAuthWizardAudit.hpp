#ifndef FIRMIUS_AUDITS_OAUTHWIZARDAUDIT_HPP
#define FIRMIUS_AUDITS_OAUTHWIZARDAUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

class OAuthWizardAudit final : public shared::IAudit {
public:
    std::string getId() const override;
    std::string getDescription() const override;
    shared::AuditResult run(const std::vector<std::string>& args) override;
};

}

#endif
