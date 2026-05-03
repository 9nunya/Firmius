#ifndef FIRMIUS_CORE_MODES_MODE_HPP
#define FIRMIUS_CORE_MODES_MODE_HPP

#include "Context.hpp" // for ToolScope

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core::modes {

using firmius::shared::ToolScope;

/**
 * @brief A single mode contract loaded from prompts/modes/<name>.md
 *
 * Modes are operational stances inside personas. Each mode supplies a
 * sub-prompt overlay, a tool scope override, a structured-output schema
 * hint, default Pact contract requirements, and the set of transitions
 * the agent may request. Personas inherit the active mode when delegated.
 *
 * Day-1 ship: data model + registry + loader. Day-3 ships:
 *   - mode_switch tool wiring
 *   - tool dispatcher enforcement of allow/deny
 *   - Pact template wiring
 *   - mode-aware welcome screen + status band
 */
struct Mode {
  std::string name;                            ///< machine name, e.g. "diagnose" or "apply"
  std::string title;                           ///< display, e.g. "Diagnose"
  std::string glyph;                           ///< single-char/emoji glyph
  std::string shortDescription;                ///< one-liner for pickers
  std::string promptOverlay;                   ///< body of the mode .md file
  std::vector<std::string> applicablePersonas; ///< empty = all personas (system-scoped)
  std::vector<ToolScope> allowScopes;          ///< intersected with persona scope
  std::vector<ToolScope> denyScopes;           ///< hard-deny override
  std::optional<std::string> outputSchema;     ///< name of expected return shape
  std::vector<std::string> autoWorkflowsOnEnter; ///< workflows to trigger
  std::vector<std::string> allowedTransitionsTo; ///< names of mode targets (qualified)
  /// done_when items the Pact subsystem may default to. Strings are the
  /// raw spec (e.g. "exit_code:0:cmake --build build"). Day-3 parses them.
  std::vector<std::string> pactDoneWhenDefaults;
  std::string sourcePath;                      ///< filesystem path

  /// Persona-scoped sub-modes (loaded from prompts/modes/<persona>/<name>.md)
  /// have personaScope set to the owning persona. System modes (loaded from
  /// prompts/modes/<name>.md) leave it empty.
  std::optional<std::string> personaScope;

  /// Optional umbrella system mode this sub-mode lives within. When the
  /// agent is in `forge:apply`, parentMode = "execute" means the system
  /// mode "execute" is also conceptually active for higher-level routing.
  std::optional<std::string> parentMode;

  /// Qualified name used by mode_switch and the prompt composer.
  /// system mode "diagnose"  -> "diagnose"
  /// forge sub-mode "apply"  -> "forge:apply"
  std::string qualifiedName() const {
    if (personaScope.has_value() && !personaScope->empty()) {
      return *personaScope + ":" + name;
    }
    return name;
  }

  /// True if this mode is owned by a specific persona (not a system mode).
  bool isPersonaScoped() const {
    return personaScope.has_value() && !personaScope->empty();
  }
};

/**
 * @brief Loader + registry for modes.
 *
 * Modes live in two layouts:
 *   - System modes: `prompts/modes/<name>.md` (e.g. diagnose, execute).
 *     Applicable to all personas listed in the file's `applicable_personas`
 *     frontmatter (empty = all).
 *   - Persona sub-modes: `prompts/modes/<persona>/<name>.md` (e.g.
 *     prompts/modes/forge/apply.md). Only valid when the named persona is
 *     active; auto-tagged with personaScope on load.
 *
 * Both layouts compose: an agent can be in `forge:apply` and treat its
 * `parent_mode: execute` as the umbrella system stance simultaneously.
 */
class ModeRegistry {
public:
  /// Load all modes from the standard prompts/modes/ directory.
  static ModeRegistry &instance();

  /// Re-read mode files from disk. Safe to call at runtime.
  void reload();

  /// Look up a mode by qualified name (e.g. "diagnose" or "forge:apply").
  const Mode *find(const std::string &qualifiedName) const;

  /// Resolve a possibly-bare name in the context of an active persona.
  /// "apply" + persona "forge"  -> finds "forge:apply" if it exists,
  ///                               otherwise falls back to system "apply".
  /// "diagnose" + any persona   -> finds system "diagnose".
  /// "forge:apply" + any        -> finds "forge:apply" verbatim.
  const Mode *resolveForPersona(const std::string &name,
                                const std::string &persona) const;

  /// All registered modes, sorted by qualified name.
  std::vector<std::string> listNames() const;

  /// Sub-modes scoped to a single persona (qualified names).
  std::vector<std::string> listForPersona(const std::string &persona) const;

  /// Number of loaded modes.
  std::size_t size() const { return modes_.size(); }

private:
  ModeRegistry() = default;
  std::map<std::string, Mode> modes_;
};

/**
 * @brief Resolve the modes directory using the same search order as
 * PurposeLoader::resolvePromptsDir() with `/modes/` appended.
 */
std::string resolveModesDir();

/**
 * @brief Parse a single mode file. Throws on schema violations.
 */
Mode loadModeFromFile(const std::string &path);

} // namespace firmius::core::modes

#endif
