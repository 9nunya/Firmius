#ifndef FIRMIUS_CORE_PURPOSE_LOADER_HPP
#define FIRMIUS_CORE_PURPOSE_LOADER_HPP

#include "Context.hpp"
#include "Enums.hpp"
#include "IProvider.hpp"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core {

using namespace firmius::shared;

enum class PurposeWorkRole {
  Lead,
  Executor,
  Worker,
  Auditor,
  Scout,
  Unknown
};

std::string purposeWorkRoleToString(PurposeWorkRole role);
PurposeWorkRole purposeWorkRoleFromString(const std::string &role);

/**
 * @brief Representation of an agent persona loaded from Markdown/YAML.
 */
struct Persona {
  std::string name;        ///< Machine name of the persona.
  std::string title;       ///< Display title.
  std::string description; ///< High-level description.
  std::string purposeKey;  ///< Immutable purpose identity used for fallback.
  std::vector<ToolScope>
      allowedScopes; ///< Tools the persona is allowed to use.
  bool canSpawn = false;      ///< Whether this persona can spawn sub-agents.
  bool switchable = false;    ///< Whether this persona is selectable as lead.
  bool hasWorkRole = false;   ///< Whether the persona declares explicit semantics.
  PurposeWorkRole workRole = PurposeWorkRole::Unknown;
  std::string identityPrompt; ///< The core instructions for the persona.
};

/**
 * @brief Loader and composer for agent personas.
 */
class PurposeLoader {
public:
  /**
   * @brief Checks if a persona exists.
   */
  static bool isValid(const std::string &purpose);

  /**
   * @brief Loads a persona from the prompts/ directory.
   * @param purpose The name of the persona file (without .md).
   * @return The loaded Persona struct.
   */
  static Persona load(const std::string &purpose);

  /**
   * @brief Resolves the runtime work role for a loaded persona, using explicit
   * metadata when present and compatibility fallbacks otherwise.
   */
  static PurposeWorkRole resolveWorkRole(const Persona &persona);

  /**
   * @brief Resolves the runtime work role for a purpose name.
   */
  static PurposeWorkRole resolveWorkRole(const std::string &purpose);

  /**
   * @brief Composes the final system prompt for an agent.
   */
  static std::string composeSystemPrompt(const Persona &persona,
                                         const AgentContext &context,
                                         const std::string &toolsBlock);

  /**
   * @brief Formats a list of tool definitions into a Markdown block.
   */
  static std::string
  buildToolsBlock(const std::vector<firmius::provider::ToolDefinition> &tools);

  /**
   * @brief Loads the compaction prompt from the prompts/ directory.
   */
  static std::string loadCompactionPrompt();

  /**
   * @brief Resolves the prompts directory using the resolution chain:
   *        $FIRMIUS_PROMPTS_DIR env var → readable ~/.firmius/prompts/ →
   *        readable ./prompts/
   * @return The path to the prompts directory with trailing slash.
   */
  static std::string resolvePromptsDir();

  /**
   * @brief Lists persona names that are marked as switchable.
   */
  static std::vector<std::string> listSwitchablePurposes();

  /**
   * @brief Lists all valid persona names found in prompts.
   */
  static std::vector<std::string> listPurposes();

  /**
   * @brief Registers a custom placeholder for system prompt composition.
   * @param key The placeholder key (e.g., "{{MY_VAR}}").
   * @param value The value to replace it with.
   */
  static void registerPlaceholder(const std::string &key,
                                  const std::string &value);

  /**
   * @brief Bootstraps default prompts by copying builtin prompts to
   * ~/.firmius/prompts/
   * @param builtinPromptsDir The path to the builtin prompts directory.
   */
  static void bootstrapDefaults(const std::string &builtinPromptsDir);

private:
  static std::map<std::string, std::string> customPlaceholders;
};

} // namespace firmius::core

#endif
