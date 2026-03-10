#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include <stdexcept>

namespace firmius::core {

AgentPermissionChecks::AgentPermissionChecks(
    const firmius::shared::AgentContext &context)
    : context_(context) {}

bool AgentPermissionChecks::checkPathAccess(
    const std::string &absolutePath) const {
  for (const auto &p : context_.permissions.allowedPaths) {
    if (firmius::shared::FSUtil::isSubpath(absolutePath, p)) {
      return true;
    }
  }
  return context_.permissions.allowOutsideCwd;
}

void AgentPermissionChecks::validatePathAccess(
    const std::string &absolutePath) const {
  if (!checkPathAccess(absolutePath)) {
    throw std::runtime_error(
        "Access denied: path outside allowed directories: " + absolutePath +
        ". You are allowed to access: " +
        firmius::shared::StringUtil::concat(context_.permissions.allowedPaths,
                                            ", "));
  }
}

} // namespace firmius::core
