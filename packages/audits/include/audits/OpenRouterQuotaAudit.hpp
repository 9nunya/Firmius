#ifndef FIRMIUS_AUDITS_OPENROUTERQUOTAAUDIT_HPP
#define FIRMIUS_AUDITS_OPENROUTERQUOTAAUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

class OpenRouterQuotaAudit final : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif // FIRMIUS_AUDITS_OPENROUTERQUOTAAUDIT_HPP
