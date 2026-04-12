#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace firmius::audits::cli {

inline std::string canonicalAuditId(std::string_view auditId) {
  if (auditId == "provider_stream_debug" ||
      auditId == "provider_live_agent") {
    return "provider_full_range";
  }
  return std::string(auditId);
}

inline std::vector<std::string>
normalizeAuditArgs(std::string_view requestedAuditId,
                   const std::vector<std::string> &auditArgs) {
  std::vector<std::string> normalized = auditArgs;
  if (requestedAuditId == "provider_live_agent") {
    const bool hasLiveAgentFlag = std::any_of(
        normalized.begin(), normalized.end(), [](const std::string &arg) {
          return arg == "--live-agent" || arg == "--harness-live";
        });
    if (!hasLiveAgentFlag) {
      normalized.insert(normalized.begin(), "--live-agent");
    }
  }
  return normalized;
}

} // namespace firmius::audits::cli
