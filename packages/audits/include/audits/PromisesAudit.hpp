#pragma once

#include "IAudit.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

class PromisesAudit final : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits
