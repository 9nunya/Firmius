#ifndef FIRMIUS_AUDITS_EMBEDDINGAUDIT_HPP
#define FIRMIUS_AUDITS_EMBEDDINGAUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

class EmbeddingAudit final : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif
