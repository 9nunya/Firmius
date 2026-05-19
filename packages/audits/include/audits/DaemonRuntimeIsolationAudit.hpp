#ifndef FIRMIUS_AUDITS_DAEMONRUNTIMEISOLATIONAUDIT_HPP
#define FIRMIUS_AUDITS_DAEMONRUNTIMEISOLATIONAUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

class DaemonRuntimeIsolationAudit final : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif // FIRMIUS_AUDITS_DAEMONRUNTIMEISOLATIONAUDIT_HPP
