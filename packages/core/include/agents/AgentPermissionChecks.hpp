#ifndef FIRMIUS_CORE_AGENT_PERMISSION_CHECKS_HPP
#define FIRMIUS_CORE_AGENT_PERMISSION_CHECKS_HPP

#include "Context.hpp"
#include <string>

namespace firmius::core {

/**
 * @brief Handles path permissions and validation checks for an agent.
 */
class AgentPermissionChecks {
public:
  /**
   * @brief Constructs the permission checks with a reference to the agent's
   * context.
   * @param context The context containing the agent's permissions.
   */
  explicit AgentPermissionChecks(const firmius::shared::AgentContext &context);

  /**
   * @brief Checks if the agent has access to the given absolute path.
   * @param absolutePath The absolute file or directory path.
   * @return true if access is allowed, false otherwise.
   */
  bool checkPathAccess(const std::string &absolutePath) const;

  /**
   * @brief Validates path access and throws a runtime_error if denied.
   * @param absolutePath The absolute file or directory path.
   * @throws std::runtime_error if access is denied.
   */
  void validatePathAccess(const std::string &absolutePath) const;

private:
  const firmius::shared::AgentContext &context_;
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_AGENT_PERMISSION_CHECKS_HPP
