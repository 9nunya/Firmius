#ifndef FIRMIUS_CORE_PERMISSIONSUGGESTIONENGINE_HPP
#define FIRMIUS_CORE_PERMISSIONSUGGESTIONENGINE_HPP

#include "environment/PermissionPolicy.hpp"
#include "environment/PolicyEngine.hpp"
#include "ICommandIntent.hpp"

#include <string>
#include <vector>

namespace firmius::core {

/// One suggestion the overlay can offer to the user. Each suggestion
/// describes a rule the system would CREATE if the user accepted it.
/// The label is what the user sees; the rule is what gets persisted.
struct PermissionSuggestion {
  std::string label;           ///< Human-readable: "Allow `cmake build`".
  std::string explanation;     ///< Optional second line: "matches ^cmake (build|...)".
  PolicyRule rule;             ///< The rule that would be inserted.
  bool defaultSelected = false; ///< Pre-checked in the picker.
};

/// Generates rule suggestions tailored to a specific permission request.
/// One method per category; pick based on `request.category`.
class PermissionSuggestionEngine {
public:
  /// Top-level dispatcher. Inspects request.category and routes.
  static std::vector<PermissionSuggestion> generate(
      const PolicyRequest &request,
      const shared::CommandIntent &intent);

  // Per-category generators (exposed for testing).
  static std::vector<PermissionSuggestion> forProcessExec(
      const PolicyRequest &request, const shared::CommandIntent &intent);
  static std::vector<PermissionSuggestion> forProcessCwd(
      const PolicyRequest &request);
  static std::vector<PermissionSuggestion> forFileRead(
      const PolicyRequest &request);
  static std::vector<PermissionSuggestion> forFileWrite(
      const PolicyRequest &request);
  static std::vector<PermissionSuggestion> forFileCreate(
      const PolicyRequest &request);
  static std::vector<PermissionSuggestion> forFileDelete(
      const PolicyRequest &request);
  static std::vector<PermissionSuggestion> forNetworkFetch(
      const PolicyRequest &request);
  static std::vector<PermissionSuggestion> forNetworkSearch(
      const PolicyRequest &request);
  static std::vector<PermissionSuggestion> forAgentSpawn(
      const PolicyRequest &request);
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_PERMISSIONSUGGESTIONENGINE_HPP
