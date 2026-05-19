#ifndef FIRMIUS_AUDITS_CACHEAUDIT_HPP
#define FIRMIUS_AUDITS_CACHEAUDIT_HPP

#include "IAudit.hpp"
#include <string>
#include <vector>

namespace firmius::audits {

/**
 * @brief Verifies prompt-caching works across configured providers.
 *
 * Sends a 2k-token system prompt + small user message twice in a row
 * to each provider/model pair. On the second call, verifies that
 * cacheRead > 0 (or cached_tokens > 0). Reports the discount achieved
 * and whether the cache hit landed.
 */
class CacheAudit : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

}  // namespace firmius::audits

#endif  // FIRMIUS_AUDITS_CACHEAUDIT_HPP
