#ifndef FIRMIUS_AUDITS_RESUMETODOAUDIT_HPP
#define FIRMIUS_AUDITS_RESUMETODOAUDIT_HPP

#include "IAudit.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

class ResumeTodoAudit : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif // FIRMIUS_AUDITS_RESUMETODOAUDIT_HPP
