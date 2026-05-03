#ifndef FIRMIUS_CORE_HOOKS_TEMPLATE_ENGINE_HPP
#define FIRMIUS_CORE_HOOKS_TEMPLATE_ENGINE_HPP

#include <map>
#include <string>

namespace firmius::core::hooks {

/**
 * @brief Resolution context for `{{...}}` template substitution.
 *
 * Each field is JSON. The engine walks dotted paths into them so authors
 * write `{{state.thread.promise.iteration}}` and the resolver navigates
 * `stateJson`'s `thread.promise.iteration` value.
 *
 * The `extras` map holds flat key→value overrides used for ergonomic
 * shorthands (`{{persona}}`, `{{thread_id}}`, etc. — pre-bound by the
 * dispatcher when building the context).
 *
 * Builtins resolved by the engine itself, no context lookup needed:
 *   - `{{ulid()}}` — Crockford-base32 26-char id, monotonic per-process.
 *   - `{{uuid()}}` — RFC 4122 v4 random UUID.
 *   - `{{now}}`    — ISO 8601 UTC timestamp.
 *
 * Filter syntax is intentionally minimal:
 *   - `{{x | default: 'fallback'}}`     — substitute when x is empty/null
 *   - `{{x | yaml}}`                     — render a JSON value as YAML
 *   - `{{x | length}}`                   — length of array/string
 *
 * Anything more advanced (loops, conditionals) is the job of a Luau
 * `kind: script` action — the templater stays small and predictable.
 */
struct TemplateContext {
  std::string stateJson = "{}";
  std::string eventJson = "{}";
  std::string toolArgsJson = "{}";
  std::string subagentReturnJson = "{}";
  std::map<std::string, std::string> extras;
};

/**
 * @brief Resolve all `{{...}}` substitutions in `body` against `ctx`.
 *
 * Unknown variables resolve to an empty string. Filters that fail also
 * resolve to empty — hooks should not blow up on missing data; a missing
 * value is a signal that the upstream chain didn't write what we
 * expected, surfaced separately by `firmius hooks log`.
 */
std::string renderTemplate(const std::string &body,
                           const TemplateContext &ctx);

/**
 * @brief Convenience helper: build a context from the standard inputs.
 *
 * The dispatcher calls this each time it fires a hook. `subagentReturnJson`
 * is empty unless the action was an Agent spawn; `extras` holds the
 * flat-key shorthands (`persona`, `thread_id`, `tool`, etc.).
 */
TemplateContext makeTemplateContext(
    const std::string &stateJson,
    const std::string &eventJson,
    const std::string &toolArgsJson = "{}",
    const std::string &subagentReturnJson = "{}",
    std::map<std::string, std::string> extras = {});

} // namespace firmius::core::hooks

#endif
