#ifndef FIRMIUS_AUDITS_REASONINGTRACEAUDIT_HPP
#define FIRMIUS_AUDITS_REASONINGTRACEAUDIT_HPP

#include "IAudit.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

class ReasoningTraceAudit final : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif // FIRMIUS_AUDITS_REASONINGTRACEAUDIT_HPP
