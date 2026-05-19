#ifndef FIRMIUS_AUDITS_PERSISTENCESTRESSAUDIT_HPP
#define FIRMIUS_AUDITS_PERSISTENCESTRESSAUDIT_HPP

#include "IAudit.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

// Synthetic stress test that measures persistence (Journaler / ThreadManager)
// throughput, cold-reload latency, and multi-agent fleet contention against a
// scratch FIRMIUS_HOME so the user's real config is untouched.
class PersistenceStressAudit : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif
