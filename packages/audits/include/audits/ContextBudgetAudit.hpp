#ifndef FIRMIUS_AUDITS_CONTEXT_BUDGET_AUDIT_HPP
#define FIRMIUS_AUDITS_CONTEXT_BUDGET_AUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

class ContextBudgetAudit : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif
