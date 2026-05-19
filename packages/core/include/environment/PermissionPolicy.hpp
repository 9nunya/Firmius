#ifndef FIRMIUS_CORE_PERMISSIONPOLICY_HPP
#define FIRMIUS_CORE_PERMISSIONPOLICY_HPP

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core {

// ── Categories ─────────────────────────────────────────────────────────
//
// Every permissioned operation is classified into one of these categories.
// The category is the primary key for routing requests to suggestion
// engines and matching against rules. Categories are stable wire strings
// so the JSON file remains forward-compatible.

inline constexpr const char *kCatFileRead    = "file.read";
inline constexpr const char *kCatFileWrite   = "file.write";
inline constexpr const char *kCatFileCreate  = "file.create";
inline constexpr const char *kCatFileDelete  = "file.delete";
inline constexpr const char *kCatProcessExec = "process.exec";
inline constexpr const char *kCatProcessCwd  = "process.cwd";
inline constexpr const char *kCatNetworkFetch = "network.fetch";
inline constexpr const char *kCatNetworkSearch = "network.search";
inline constexpr const char *kCatAgentSpawn  = "agent.spawn";
inline constexpr const char *kCatArtifactWrite = "artifact.write";

/// Three-valued decision from policy evaluation.
enum class PolicyDecision {
  Allow,   ///< Rule explicitly allows. No prompt.
  Deny,    ///< Rule explicitly denies. No prompt — operation rejected.
  Ask,     ///< No matching allow/deny — fall through to escalation prompt.
};

/// Where a rule was sourced from. Influences scope and persistence.
enum class RuleScope {
  /// Lives in `<project>/.firmius/permissions.json`. Project-local.
  Project,
  /// Lives in `~/.firmius/permissions.json`. Cross-project.
  Global,
  /// Lives in memory only. Disappears at process exit. Created by the
  /// "allow for this session" UI options.
  Session,
};

/// One rule entry. The `match` map is category-specific and AND-ed:
/// every key must match for the rule to apply. Empty match map means
/// "match everything in this category" (rare — used for category-wide
/// auto-allow policies).
struct PolicyRule {
  std::string id;                ///< UUID-ish. Used for revoke/upsert.
  std::string category;          ///< One of kCat* constants.
  std::map<std::string, std::string> match;  ///< Key → glob OR regex.
  PolicyDecision decision = PolicyDecision::Allow;
  RuleScope scope = RuleScope::Global;
  std::string comment;           ///< Optional human note.
  std::uint64_t createdAt = 0;   ///< Unix ms.
  /// Optional TTL (unix ms expiry). 0 = no expiry.
  std::uint64_t expiresAt = 0;
  /// Empty = global rule (applies regardless of active mode). Otherwise
  /// the id of the mode this rule belongs to. Set when the rule is
  /// crafted by the user from an "Allow Always" picker — picks land in
  /// the active mode's bucket so switching modes wipes the choices.
  std::string modeId;

  bool operator==(const PolicyRule &) const = default;
};

/// Per-category default decision when no rule matches.
struct CategoryDefaults {
  std::map<std::string, PolicyDecision> byCategory;

  bool operator==(const CategoryDefaults &) const = default;
};

/// A user-named permission profile. Each mode carries its own
/// CategoryDefaults override (e.g. yolo flips every category to Allow)
/// AND owns a slice of rules tagged with its modeId. Switching modes
/// changes which slice is active.
struct PermissionMode {
  std::string id;                ///< Stable string id ("ask", "yolo", uuid).
  std::string name;              ///< Display name (mutable).
  /// Per-category overrides. Falls back to the document defaults when
  /// a category isn't in the map. Lets the yolo mode flip everything to
  /// Allow with one entry instead of N rules.
  CategoryDefaults categoryDefaults;
  std::string description;       ///< Optional human note.
  /// True if this is a built-in seed mode that shouldn't be deletable.
  /// Set on `ask` (empty rule mode) so the user can't lock themselves
  /// out by accident.
  bool builtIn = false;

  bool operator==(const PermissionMode &) const = default;
};

/// Top-level snapshot. Loaded from JSON, mutated in memory, written back.
struct PolicyDocument {
  int version = 2;
  PolicyDecision defaultDecision = PolicyDecision::Ask;
  CategoryDefaults categoryDefaults;
  std::vector<PolicyRule> rules;

  /// User-defined permission modes. Each mode bundles a name, a
  /// category-default override, and (via PolicyRule::modeId) a private
  /// slice of the rules vector. Built-in seed: `ask` (empty), `yolo`
  /// (every category Allow). Users can add more via /permissions.
  std::vector<PermissionMode> modes;
  /// Id of the currently active mode. Empty falls back to `ask`.
  std::string activeModeId;

  /// File this document was loaded from (empty for in-memory only).
  std::filesystem::path source;
  std::filesystem::file_time_type lastWrite{};

  bool operator==(const PolicyDocument &) const = default;
};

// ── Wire helpers ───────────────────────────────────────────────────────

const char *decisionToWire(PolicyDecision d);
PolicyDecision decisionFromWire(const std::string &s);

const char *scopeToWire(RuleScope s);
RuleScope scopeFromWire(const std::string &s);

/// Built-in default category decisions (only applied when JSON omits them).
CategoryDefaults defaultCategoryDefaults();

/// Seed modes shipped with every fresh user policy. Always returns at
/// least `ask` (empty) and `yolo` (allow-all). Built-in flag set so the
/// CRUD layer refuses to delete them by id.
std::vector<PermissionMode> defaultSeedModes();

/// Constants for the seed mode ids.
inline constexpr const char *kModeAsk  = "ask";
inline constexpr const char *kModeYolo = "yolo";

} // namespace firmius::core

#endif // FIRMIUS_CORE_PERMISSIONPOLICY_HPP
