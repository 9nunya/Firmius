#ifndef FIRMIUS_AUDITS_TUI_RUNTIME_STRESS_AUDIT_HPP
#define FIRMIUS_AUDITS_TUI_RUNTIME_STRESS_AUDIT_HPP

#include "IAudit.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

class TuiRuntimeStressAudit : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif
