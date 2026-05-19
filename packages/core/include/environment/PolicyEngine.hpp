#ifndef FIRMIUS_CORE_POLICYENGINE_HPP
#define FIRMIUS_CORE_POLICYENGINE_HPP

#include "environment/PermissionPolicy.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core {

/// One concrete request being evaluated. Carries enough context for
/// rules to match and for the suggestion engine to generate options.
struct PolicyRequest {
  std::string category;          ///< kCat* constant.

  // ── File-flavored ──
  std::string path;
  bool isDirectory = false;
  std::string toolName;

  // ── Process-flavored ──
  std::string command;
  std::string commandPrimary;
  std::string cwd;
  /// Pre-parsed subcommands (for pipelined / `&&`-joined commands).
  /// First element is the same as `commandPrimary` of subcmd 0; the
  /// list lets the suggestion engine offer per-subcommand grants.
  std::vector<std::string> subcommands;

  // ── Network-flavored ──
  std::string url;
  std::string host;
  std::string scheme;
  std::string query;     ///< For network.search.

  // ── Agent-flavored ──
  std::string persona;
  std::string parentPersona;
  std::vector<std::string> toolScopes;
};

/// Result of evaluating a request against the policy.
struct PolicyEvaluation {
  PolicyDecision decision = PolicyDecision::Ask;
  /// The rule that matched (if decision came from a specific rule).
  std::optional<PolicyRule> matchedRule;
  /// True if the decision came from a category default (not a rule).
  bool fromCategoryDefault = false;
  /// True if from the document-wide default_decision.
  bool fromDocumentDefault = false;
  std::string reason;            ///< Human-readable explanation.
};

/// Owns the loaded policy and provides eval + edit operations.
/// Thread-safe.
class PolicyEngine {
public:
  /// Construct with a base path for the user-level policy file
  /// (`~/.firmius/permissions.json` by default, override for tests).
  /// `projectPath` is the optional working directory whose
  /// `.firmius/permissions.json` overlays the user policy.
  PolicyEngine(std::filesystem::path userPolicyPath = {},
               std::filesystem::path projectPath = {});

  /// Default user policy file location.
  static std::filesystem::path defaultUserPolicyPath();

  /// Reload the on-disk policy if the file mtime changed since last
  /// read. Called automatically before evaluate().
  void maybeReload();

  /// Force reload, regardless of mtime.
  void forceReload();

  /// Evaluate a request against the merged policy.
  /// Includes session rules from the in-memory layer.
  PolicyEvaluation evaluate(const PolicyRequest &req);

  // ── Rule mutation ──

  /// Append a rule. If `id` is empty, generates one. Returns the id.
  /// If `scope` is Session, the rule lives in memory only.
  /// If Global / Project, persists to the corresponding JSON file.
  std::string upsertRule(PolicyRule rule);

  /// Remove a rule by id from any scope. Returns true if removed.
  bool removeRule(const std::string &id);

  /// Remove all rules with the given category.
  void clearCategory(const std::string &category);

  /// Snapshot all rules across scopes (project + global + session).
  std::vector<PolicyRule> listRules() const;

  // ── Mode CRUD ─────────────────────────────────────────────────────

  /// Snapshot all modes (built-ins + user-created).
  std::vector<PermissionMode> listModes() const;

  /// Get the currently-active mode. Falls back to `ask` if the stored
  /// activeModeId points at a deleted/missing mode.
  PermissionMode activeMode() const;

  /// Switch the active mode. Returns false if `id` doesn't exist.
  bool setActiveMode(const std::string &id);

  /// Create a new user mode. If `seedFromActive` is true, copies the
  /// active mode's rules and category defaults as a starting point.
  /// Returns the id of the new mode (matches `mode.id` if non-empty,
  /// otherwise generated). On name/id collision, returns "".
  std::string createMode(PermissionMode mode, bool seedFromActive = false);

  /// Rename. Returns false on missing-id or built-in.
  bool renameMode(const std::string &id, const std::string &newName);

  /// Delete a mode and all its mode-scoped rules. Returns false on
  /// missing-id, built-in, or attempted deletion of the active mode.
  bool deleteMode(const std::string &id);

  /// Snapshot the user-level document (excluding session-only rules).
  PolicyDocument userDocument() const;

  /// Snapshot the project-level document (or empty if none).
  PolicyDocument projectDocument() const;

  /// Convenience: are there any pending session rules? (For UI.)
  bool hasSessionRules() const;

  /// Drop all session rules. Called on thread switch / session end.
  void clearSessionRules();

  /// Persist the current user document back to disk. Idempotent —
  /// safe to call after every upsert.
  void writeUser();

  /// Persist the current project document back to disk.
  void writeProject();

  /// Path getters.
  std::filesystem::path userPolicyPath() const { return userPolicyPath_; }
  std::filesystem::path projectPolicyPath() const { return projectPolicyPath_; }

  // ── Migration ──

  /// One-shot migration: hydrate legacy rules from ThreadManager into
  /// the user JSON document. Idempotent — checks a flag in the doc.
  /// Pass a list of (categoryHint, rule) pairs derived externally.
  /// Returns the number of rules hydrated.
  int hydrateLegacyRules(const std::vector<PolicyRule> &legacyRules);

private:
  mutable std::recursive_mutex mutex_;
  std::filesystem::path userPolicyPath_;
  std::filesystem::path projectPolicyPath_;
  PolicyDocument userDoc_;
  PolicyDocument projectDoc_;
  std::vector<PolicyRule> sessionRules_;

  /// Match a single rule's `match` map against the request.
  bool matchRule(const PolicyRule &rule, const PolicyRequest &req) const;

  /// Match one match-key against a request (path_glob, command_regex, ...).
  bool matchKey(const std::string &key, const std::string &pattern,
                const PolicyRequest &req) const;

  void load(PolicyDocument &doc, const std::filesystem::path &path);
  void save(const PolicyDocument &doc, const std::filesystem::path &path);
  static PolicyDocument parse(const std::string &json,
                              const std::filesystem::path &source);
  static std::string serialize(const PolicyDocument &doc);
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_POLICYENGINE_HPP
