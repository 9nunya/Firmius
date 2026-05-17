#ifndef FIRMIUS_AUDITS_EMBEDDING_AUDIT_HPP
#define FIRMIUS_AUDITS_EMBEDDING_AUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

class EmbeddingAudit final : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;

private:
  static double cosineSimilarity(const std::vector<float> &a,
                                 const std::vector<float> &b);
};

} // namespace firmius::audits

#endif
