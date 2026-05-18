#include "environment/PermissionPolicy.hpp"
#include "environment/PolicyEngine.hpp"

#include "utils/GlobMatch.hpp"
#include "utils/PlatformPaths.hpp"
#include "utils/StringUtil.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/prettywriter.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace firmius::core {

// ── Decision / scope wire helpers ─────────────────────────────────────

const char *decisionToWire(PolicyDecision d) {
  switch (d) {
  case PolicyDecision::Allow: return "allow";
  case PolicyDecision::Deny:  return "deny";
  case PolicyDecision::Ask:   return "ask";
  }
  return "ask";
}

PolicyDecision decisionFromWire(const std::string &s) {
  if (s == "allow") return PolicyDecision::Allow;
  if (s == "deny")  return PolicyDecision::Deny;
  return PolicyDecision::Ask;
}

const char *scopeToWire(RuleScope s) {
  switch (s) {
  case RuleScope::Project: return "project";
  case RuleScope::Global:  return "global";
  case RuleScope::Session: return "session";
  }
  return "global";
}

RuleScope scopeFromWire(const std::string &s) {
  if (s == "project") return RuleScope::Project;
  if (s == "session") return RuleScope::Session;
  return RuleScope::Global;
}

CategoryDefaults defaultCategoryDefaults() {
  CategoryDefaults d;
  // Conservative defaults: most categories ask. Network search is auto
  // because it's read-only and low-risk. Agent spawn auto-allows because
  // most subagents are intentional fan-outs and prompting for every one
  // is annoying. Artifact write is internal storage, auto.
  d.byCategory[kCatFileRead]    = PolicyDecision::Ask;
  d.byCategory[kCatFileWrite]   = PolicyDecision::Ask;
  d.byCategory[kCatFileCreate]  = PolicyDecision::Ask;
  d.byCategory[kCatFileDelete]  = PolicyDecision::Ask;
  d.byCategory[kCatProcessExec] = PolicyDecision::Ask;
  d.byCategory[kCatProcessCwd]  = PolicyDecision::Ask;
  d.byCategory[kCatNetworkFetch] = PolicyDecision::Ask;
  d.byCategory[kCatNetworkSearch] = PolicyDecision::Allow;
  d.byCategory[kCatAgentSpawn]  = PolicyDecision::Allow;
  d.byCategory[kCatArtifactWrite] = PolicyDecision::Allow;
  return d;
}

std::vector<PermissionMode> defaultSeedModes() {
  std::vector<PermissionMode> modes;

  // `ask` — empty rule slice, defaults from the document. Is the
  // landing pad for AllowAlways picks until the user creates a custom
  // mode. The user starts here on a fresh install.
  PermissionMode ask;
  ask.id = kModeAsk;
  ask.name = "ask";
  ask.description = "Default mode. Asks before risky actions; learns from your choices.";
  ask.builtIn = true;
  modes.push_back(std::move(ask));

  // `yolo` — every category flipped to Allow. Useful for sandboxed
  // throwaway threads or intentional unattended runs.
  PermissionMode yolo;
  yolo.id = kModeYolo;
  yolo.name = "yolo";
  yolo.description = "Allow everything without prompting. Use with caution.";
  yolo.builtIn = true;
  yolo.categoryDefaults.byCategory[kCatFileRead]    = PolicyDecision::Allow;
  yolo.categoryDefaults.byCategory[kCatFileWrite]   = PolicyDecision::Allow;
  yolo.categoryDefaults.byCategory[kCatFileCreate]  = PolicyDecision::Allow;
  yolo.categoryDefaults.byCategory[kCatFileDelete]  = PolicyDecision::Allow;
  yolo.categoryDefaults.byCategory[kCatProcessExec] = PolicyDecision::Allow;
  yolo.categoryDefaults.byCategory[kCatProcessCwd]  = PolicyDecision::Allow;
  yolo.categoryDefaults.byCategory[kCatNetworkFetch] = PolicyDecision::Allow;
  yolo.categoryDefaults.byCategory[kCatNetworkSearch] = PolicyDecision::Allow;
  yolo.categoryDefaults.byCategory[kCatAgentSpawn]  = PolicyDecision::Allow;
  yolo.categoryDefaults.byCategory[kCatArtifactWrite] = PolicyDecision::Allow;
  modes.push_back(std::move(yolo));

  return modes;
}

// ── PolicyEngine ──────────────────────────────────────────────────────

namespace {

std::uint64_t nowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path projectPolicyFor(const std::filesystem::path &projectPath) {
  if (projectPath.empty()) return {};
  return projectPath / ".firmius" / "permissions.json";
}

bool isExpired(const PolicyRule &r) {
  return r.expiresAt > 0 && r.expiresAt < nowMs();
}

// Try a value as a regex first, fall back to glob if regex fails to
// compile or if the key explicitly says "_glob". Pattern keys ending in
// `_regex` are always treated as regex; `_glob` always as glob.
bool patternMatches(const std::string &key, const std::string &pattern,
                    const std::string &value) {
  const bool isRegex = key.size() >= 6 &&
                       key.substr(key.size() - 6) == "_regex";
  const bool isGlob  = key.size() >= 5 &&
                       key.substr(key.size() - 5) == "_glob";
  if (isRegex) {
    try {
      return std::regex_search(value, std::regex(pattern));
    } catch (const std::regex_error&) {
      return false;
    }
  }
  if (isGlob) {
    return shared::utils::globMatches(pattern, value);
  }
  // No suffix → exact match.
  return pattern == value;
}

} // namespace

PolicyEngine::PolicyEngine(std::filesystem::path userPolicyPath,
                           std::filesystem::path projectPath)
    : userPolicyPath_(userPolicyPath.empty() ? defaultUserPolicyPath()
                                              : std::move(userPolicyPath)),
      projectPolicyPath_(projectPolicyFor(projectPath)) {
  forceReload();
}

std::filesystem::path PolicyEngine::defaultUserPolicyPath() {
  return shared::PlatformPaths::firmiusHomeDir() / "permissions.json";
}

void PolicyEngine::maybeReload() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  // Only reload if mtime changed. Cheap stat.
  std::error_code ec;
  if (!userPolicyPath_.empty() &&
      std::filesystem::exists(userPolicyPath_, ec)) {
    auto mt = std::filesystem::last_write_time(userPolicyPath_, ec);
    if (!ec && mt != userDoc_.lastWrite) {
      load(userDoc_, userPolicyPath_);
    }
  }
  if (!projectPolicyPath_.empty() &&
      std::filesystem::exists(projectPolicyPath_, ec)) {
    auto mt = std::filesystem::last_write_time(projectPolicyPath_, ec);
    if (!ec && mt != projectDoc_.lastWrite) {
      load(projectDoc_, projectPolicyPath_);
    }
  }
}

void PolicyEngine::forceReload() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::error_code ec;
  if (!userPolicyPath_.empty() &&
      std::filesystem::exists(userPolicyPath_, ec)) {
    load(userDoc_, userPolicyPath_);
  } else {
    userDoc_ = PolicyDocument{};
    userDoc_.categoryDefaults = defaultCategoryDefaults();
    userDoc_.modes = defaultSeedModes();
    userDoc_.activeModeId = kModeAsk;
    userDoc_.source = userPolicyPath_;
  }
  // Ensure seed modes always exist even after a partial JSON edit.
  // We never delete user-added modes, just guarantee `ask` and `yolo`
  // are present.
  bool hasAsk = false, hasYolo = false;
  for (const auto &m : userDoc_.modes) {
    if (m.id == kModeAsk)  hasAsk = true;
    if (m.id == kModeYolo) hasYolo = true;
  }
  if (!hasAsk || !hasYolo) {
    auto seeds = defaultSeedModes();
    for (auto &s : seeds) {
      bool exists = false;
      for (const auto &m : userDoc_.modes) {
        if (m.id == s.id) { exists = true; break; }
      }
      if (!exists) userDoc_.modes.push_back(std::move(s));
    }
  }
  if (userDoc_.activeModeId.empty()) userDoc_.activeModeId = kModeAsk;

  if (!projectPolicyPath_.empty() &&
      std::filesystem::exists(projectPolicyPath_, ec)) {
    load(projectDoc_, projectPolicyPath_);
  } else {
    projectDoc_ = PolicyDocument{};
    projectDoc_.source = projectPolicyPath_;
  }
}

// ── Evaluation ────────────────────────────────────────────────────────

PolicyEvaluation PolicyEngine::evaluate(const PolicyRequest &req) {
  maybeReload();

  PolicyEvaluation eval;
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  // Resolve active mode (falls back to ask).
  const std::string activeId = userDoc_.activeModeId.empty()
                                     ? std::string(kModeAsk)
                                     : userDoc_.activeModeId;
  const PermissionMode *active = nullptr;
  for (const auto &m : userDoc_.modes) {
    if (m.id == activeId) { active = &m; break; }
  }

  // Build merged rule list: project first (most specific), then global,
  // then session (in-memory). Within those, a deny anywhere always
  // wins. Within decision class, first match wins.
  // Mode-scoped rules (rule.modeId == activeId) only apply when their
  // mode is active. Mode-tagged rules from a non-active mode are
  // skipped here.
  std::vector<const PolicyRule*> deniesFirst;
  std::vector<const PolicyRule*> allowsFirst;

  auto add = [&](const std::vector<PolicyRule> &rules) {
    for (const auto &r : rules) {
      if (isExpired(r)) continue;
      if (r.category != req.category) continue;
      if (!r.modeId.empty() && r.modeId != activeId) continue;
      if (!matchRule(r, req)) continue;
      if (r.decision == PolicyDecision::Deny) {
        deniesFirst.push_back(&r);
      } else if (r.decision == PolicyDecision::Allow) {
        allowsFirst.push_back(&r);
      }
    }
  };
  add(projectDoc_.rules);
  add(userDoc_.rules);
  add(sessionRules_);

  if (!deniesFirst.empty()) {
    eval.decision = PolicyDecision::Deny;
    eval.matchedRule = *deniesFirst.front();
    eval.reason = "matched deny rule: " + deniesFirst.front()->id;
    return eval;
  }
  if (!allowsFirst.empty()) {
    eval.decision = PolicyDecision::Allow;
    eval.matchedRule = *allowsFirst.front();
    eval.reason = "matched allow rule: " + allowsFirst.front()->id;
    return eval;
  }

  // Category default precedence: active mode → project doc → user doc → built-in.
  auto consult = [&](const CategoryDefaults &d) -> std::optional<PolicyDecision> {
    auto it = d.byCategory.find(req.category);
    if (it != d.byCategory.end()) return it->second;
    return std::nullopt;
  };
  if (active) {
    if (auto d = consult(active->categoryDefaults); d.has_value()) {
      eval.decision = *d;
      eval.fromCategoryDefault = true;
      eval.reason = "category default (mode: " + active->name + ")";
      if (eval.decision != PolicyDecision::Ask) return eval;
    }
  }
  if (auto d = consult(projectDoc_.categoryDefaults); d.has_value()) {
    eval.decision = *d;
    eval.fromCategoryDefault = true;
    eval.reason = "category default (project)";
    if (eval.decision != PolicyDecision::Ask) return eval;
  }
  if (auto d = consult(userDoc_.categoryDefaults); d.has_value()) {
    eval.decision = *d;
    eval.fromCategoryDefault = true;
    eval.reason = "category default (user)";
    if (eval.decision != PolicyDecision::Ask) return eval;
  }
  if (auto d = consult(defaultCategoryDefaults()); d.has_value()) {
    eval.decision = *d;
    eval.fromCategoryDefault = true;
    eval.reason = "category default (built-in)";
    if (eval.decision != PolicyDecision::Ask) return eval;
  }

  // Document-wide default.
  if (projectDoc_.defaultDecision != PolicyDecision::Ask) {
    eval.decision = projectDoc_.defaultDecision;
    eval.fromDocumentDefault = true;
    eval.reason = "document default (project)";
    return eval;
  }
  eval.decision = userDoc_.defaultDecision;
  eval.fromDocumentDefault = true;
  eval.reason = "document default (user)";
  return eval;
}

bool PolicyEngine::matchRule(const PolicyRule &rule,
                             const PolicyRequest &req) const {
  // Empty match map = "match everything" within this category.
  if (rule.match.empty()) return true;
  for (const auto &[key, pattern] : rule.match) {
    if (!matchKey(key, pattern, req)) return false;
  }
  return true;
}

bool PolicyEngine::matchKey(const std::string &key,
                            const std::string &pattern,
                            const PolicyRequest &req) const {
  // Strip suffix to find which field we're testing.
  std::string base = key;
  if (base.size() > 6 && base.substr(base.size() - 6) == "_regex")
    base.resize(base.size() - 6);
  else if (base.size() > 5 && base.substr(base.size() - 5) == "_glob")
    base.resize(base.size() - 5);

  if (base == "path")      return patternMatches(key, pattern, req.path);
  if (base == "command")   return patternMatches(key, pattern, req.command);
  if (base == "primary_command")
                            return pattern == req.commandPrimary;
  if (base == "cwd")       return patternMatches(key, pattern, req.cwd);
  if (base == "url")       return patternMatches(key, pattern, req.url);
  if (base == "host")      return patternMatches(key, pattern, req.host);
  if (base == "scheme")    return pattern == req.scheme;
  if (base == "query")     return patternMatches(key, pattern, req.query);
  if (base == "persona")   return pattern == req.persona;
  if (base == "parent_persona")
                            return pattern == req.parentPersona;
  if (base == "tool")      return pattern == req.toolName;
  if (base == "tool_scope") {
    for (const auto &s : req.toolScopes) {
      if (patternMatches(key, pattern, s)) return true;
    }
    return false;
  }
  // Unknown key → fail closed (don't match).
  return false;
}

// ── Mutation ──────────────────────────────────────────────────────────

std::string PolicyEngine::upsertRule(PolicyRule rule) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (rule.id.empty()) rule.id = "rule_" + shared::StringUtil::generateUuid();
  if (rule.createdAt == 0) rule.createdAt = nowMs();

  auto &target = rule.scope == RuleScope::Session ? sessionRules_
               : rule.scope == RuleScope::Project ? projectDoc_.rules
                                                  : userDoc_.rules;

  // Replace existing by id.
  for (auto &r : target) {
    if (r.id == rule.id) { r = rule; goto persisted; }
  }
  target.push_back(rule);

persisted:
  if (rule.scope == RuleScope::Global) writeUser();
  else if (rule.scope == RuleScope::Project) writeProject();
  return rule.id;
}

bool PolicyEngine::removeRule(const std::string &id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto removeFrom = [&](std::vector<PolicyRule> &v) {
    auto it = std::find_if(v.begin(), v.end(),
                           [&](const PolicyRule &r) { return r.id == id; });
    if (it == v.end()) return false;
    v.erase(it);
    return true;
  };
  if (removeFrom(sessionRules_)) return true;
  if (removeFrom(userDoc_.rules)) { writeUser(); return true; }
  if (removeFrom(projectDoc_.rules)) { writeProject(); return true; }
  return false;
}

void PolicyEngine::clearCategory(const std::string &category) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto wipe = [&](std::vector<PolicyRule> &v) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const PolicyRule &r) {
                             return r.category == category;
                           }),
            v.end());
  };
  wipe(sessionRules_);
  wipe(userDoc_.rules);
  wipe(projectDoc_.rules);
  writeUser();
  writeProject();
}

std::vector<PolicyRule> PolicyEngine::listRules() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<PolicyRule> out;
  out.reserve(projectDoc_.rules.size() + userDoc_.rules.size() +
              sessionRules_.size());
  for (const auto &r : projectDoc_.rules) out.push_back(r);
  for (const auto &r : userDoc_.rules)    out.push_back(r);
  for (const auto &r : sessionRules_)     out.push_back(r);
  return out;
}

// ── Mode CRUD ─────────────────────────────────────────────────────────

std::vector<PermissionMode> PolicyEngine::listModes() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return userDoc_.modes;
}

PermissionMode PolicyEngine::activeMode() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string id = userDoc_.activeModeId.empty()
                              ? std::string(kModeAsk)
                              : userDoc_.activeModeId;
  for (const auto &m : userDoc_.modes) {
    if (m.id == id) return m;
  }
  // Stored id no longer exists — fall back to ask.
  for (const auto &m : userDoc_.modes) {
    if (m.id == kModeAsk) return m;
  }
  // Should never happen (forceReload guarantees ask exists), but
  // produce a sane fallback.
  PermissionMode fallback;
  fallback.id = kModeAsk;
  fallback.name = "ask";
  fallback.builtIn = true;
  return fallback;
}

bool PolicyEngine::setActiveMode(const std::string &id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto &m : userDoc_.modes) {
    if (m.id == id) {
      userDoc_.activeModeId = id;
      writeUser();
      return true;
    }
  }
  return false;
}

std::string PolicyEngine::createMode(PermissionMode mode, bool seedFromActive) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (mode.id.empty()) {
    mode.id = "mode_" + shared::StringUtil::generateUuid();
  }
  // Reject collisions.
  for (const auto &m : userDoc_.modes) {
    if (m.id == mode.id) return "";
    if (!mode.name.empty() && m.name == mode.name) return "";
  }
  mode.builtIn = false;

  if (seedFromActive) {
    PermissionMode src = activeMode();
    if (mode.categoryDefaults.byCategory.empty()) {
      mode.categoryDefaults = src.categoryDefaults;
    }
    // Copy mode-tagged rules from the active mode under the new mode id.
    std::vector<PolicyRule> copies;
    for (const auto &r : userDoc_.rules) {
      if (r.modeId == src.id) {
        PolicyRule c = r;
        c.id = "rule_" + shared::StringUtil::generateUuid();
        c.modeId = mode.id;
        copies.push_back(std::move(c));
      }
    }
    for (auto &c : copies) userDoc_.rules.push_back(std::move(c));
  }

  userDoc_.modes.push_back(mode);
  writeUser();
  return mode.id;
}

bool PolicyEngine::renameMode(const std::string &id,
                               const std::string &newName) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto &m : userDoc_.modes) {
    if (m.id != id) continue;
    if (m.builtIn) return false;
    // Reject duplicate names.
    for (const auto &other : userDoc_.modes) {
      if (other.id != id && other.name == newName) return false;
    }
    m.name = newName;
    writeUser();
    return true;
  }
  return false;
}

bool PolicyEngine::deleteMode(const std::string &id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (id == userDoc_.activeModeId) return false;
  for (auto it = userDoc_.modes.begin(); it != userDoc_.modes.end(); ++it) {
    if (it->id != id) continue;
    if (it->builtIn) return false;
    userDoc_.modes.erase(it);
    // Drop all rules tagged for this mode.
    userDoc_.rules.erase(
        std::remove_if(userDoc_.rules.begin(), userDoc_.rules.end(),
                       [&](const PolicyRule &r) { return r.modeId == id; }),
        userDoc_.rules.end());
    writeUser();
    return true;
  }
  return false;
}

PolicyDocument PolicyEngine::userDocument() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return userDoc_;
}

PolicyDocument PolicyEngine::projectDocument() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return projectDoc_;
}

bool PolicyEngine::hasSessionRules() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return !sessionRules_.empty();
}

void PolicyEngine::clearSessionRules() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  sessionRules_.clear();
}

void PolicyEngine::writeUser() {
  if (userPolicyPath_.empty()) return;
  save(userDoc_, userPolicyPath_);
}

void PolicyEngine::writeProject() {
  if (projectPolicyPath_.empty()) return;
  if (projectDoc_.rules.empty() &&
      projectDoc_.categoryDefaults.byCategory.empty() &&
      projectDoc_.defaultDecision == PolicyDecision::Ask) {
    // Don't write empty project files.
    std::error_code ec;
    std::filesystem::remove(projectPolicyPath_, ec);
    return;
  }
  save(projectDoc_, projectPolicyPath_);
}

int PolicyEngine::hydrateLegacyRules(const std::vector<PolicyRule> &legacyRules) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  // Idempotent: skip if doc already has any rule whose id starts with
  // "legacy_". Rough but good enough — this is one-shot.
  for (const auto &r : userDoc_.rules) {
    if (r.id.rfind("legacy_", 0) == 0) return 0;
  }
  int hydrated = 0;
  for (auto rule : legacyRules) {
    if (rule.id.empty()) {
      rule.id = "legacy_" + shared::StringUtil::generateUuid();
    } else if (rule.id.rfind("legacy_", 0) != 0) {
      rule.id = "legacy_" + rule.id;
    }
    if (rule.createdAt == 0) rule.createdAt = nowMs();
    rule.scope = RuleScope::Global;
    userDoc_.rules.push_back(std::move(rule));
    ++hydrated;
  }
  if (hydrated > 0) writeUser();
  return hydrated;
}

// ── (de)serialization ─────────────────────────────────────────────────

void PolicyEngine::load(PolicyDocument &doc, const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in) {
    // Treat missing as empty.
    doc = PolicyDocument{};
    doc.source = path;
    return;
  }
  std::stringstream buf;
  buf << in.rdbuf();
  try {
    doc = parse(buf.str(), path);
  } catch (const std::exception &) {
    // Corrupt file: keep going with empty doc rather than crash.
    doc = PolicyDocument{};
    doc.source = path;
  }
  std::error_code ec;
  doc.lastWrite = std::filesystem::last_write_time(path, ec);
}

void PolicyEngine::save(const PolicyDocument &docIn,
                        const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const std::string text = serialize(docIn);
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("permissions: cannot write " + path.string());
  }
  out << text;
  out.close();
  // Refresh mtime on the in-memory copy so maybeReload doesn't think
  // someone else just touched it.
  PolicyDocument *target = nullptr;
  if (path == userPolicyPath_) target = &userDoc_;
  else if (path == projectPolicyPath_) target = &projectDoc_;
  if (target) {
    target->lastWrite = std::filesystem::last_write_time(path, ec);
  }
}

PolicyDocument PolicyEngine::parse(const std::string &json,
                                    const std::filesystem::path &source) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  PolicyDocument doc;
  doc.source = source;
  if (d.HasParseError() || !d.IsObject()) {
    doc.categoryDefaults = defaultCategoryDefaults();
    return doc;
  }
  if (d.HasMember("version") && d["version"].IsInt()) {
    doc.version = d["version"].GetInt();
  }
  if (d.HasMember("default_decision") && d["default_decision"].IsString()) {
    doc.defaultDecision = decisionFromWire(d["default_decision"].GetString());
  }
  if (d.HasMember("category_defaults") && d["category_defaults"].IsObject()) {
    for (auto it = d["category_defaults"].MemberBegin();
         it != d["category_defaults"].MemberEnd(); ++it) {
      if (it->value.IsString()) {
        doc.categoryDefaults.byCategory[it->name.GetString()] =
            decisionFromWire(it->value.GetString());
      }
    }
  } else {
    doc.categoryDefaults = defaultCategoryDefaults();
  }
  if (d.HasMember("rules") && d["rules"].IsArray()) {
    for (const auto &r : d["rules"].GetArray()) {
      if (!r.IsObject()) continue;
      PolicyRule rule;
      if (r.HasMember("id") && r["id"].IsString())
        rule.id = r["id"].GetString();
      if (r.HasMember("category") && r["category"].IsString())
        rule.category = r["category"].GetString();
      if (r.HasMember("decision") && r["decision"].IsString())
        rule.decision = decisionFromWire(r["decision"].GetString());
      if (r.HasMember("scope") && r["scope"].IsString())
        rule.scope = scopeFromWire(r["scope"].GetString());
      if (r.HasMember("comment") && r["comment"].IsString())
        rule.comment = r["comment"].GetString();
      if (r.HasMember("created_at") && r["created_at"].IsUint64())
        rule.createdAt = r["created_at"].GetUint64();
      if (r.HasMember("expires_at") && r["expires_at"].IsUint64())
        rule.expiresAt = r["expires_at"].GetUint64();
      if (r.HasMember("mode_id") && r["mode_id"].IsString())
        rule.modeId = r["mode_id"].GetString();
      if (r.HasMember("match") && r["match"].IsObject()) {
        for (auto it = r["match"].MemberBegin();
             it != r["match"].MemberEnd(); ++it) {
          if (it->value.IsString()) {
            rule.match[it->name.GetString()] = it->value.GetString();
          }
        }
      }
      // Persisted rules default to Global if scope absent.
      if (r.HasMember("scope")) {
        // already set above
      } else {
        rule.scope = RuleScope::Global;
      }
      doc.rules.push_back(std::move(rule));
    }
  }
  if (d.HasMember("active_mode_id") && d["active_mode_id"].IsString()) {
    doc.activeModeId = d["active_mode_id"].GetString();
  }
  if (d.HasMember("modes") && d["modes"].IsArray()) {
    for (const auto &m : d["modes"].GetArray()) {
      if (!m.IsObject()) continue;
      PermissionMode mode;
      if (m.HasMember("id") && m["id"].IsString())
        mode.id = m["id"].GetString();
      if (m.HasMember("name") && m["name"].IsString())
        mode.name = m["name"].GetString();
      if (m.HasMember("description") && m["description"].IsString())
        mode.description = m["description"].GetString();
      if (m.HasMember("built_in") && m["built_in"].IsBool())
        mode.builtIn = m["built_in"].GetBool();
      if (m.HasMember("category_defaults") && m["category_defaults"].IsObject()) {
        for (auto it = m["category_defaults"].MemberBegin();
             it != m["category_defaults"].MemberEnd(); ++it) {
          if (it->value.IsString()) {
            mode.categoryDefaults.byCategory[it->name.GetString()] =
                decisionFromWire(it->value.GetString());
          }
        }
      }
      doc.modes.push_back(std::move(mode));
    }
  }
  return doc;
}

std::string PolicyEngine::serialize(const PolicyDocument &doc) {
  rapidjson::Document d(rapidjson::kObjectType);
  auto &alloc = d.GetAllocator();
  d.AddMember("version", doc.version, alloc);
  d.AddMember("default_decision",
              rapidjson::Value(decisionToWire(doc.defaultDecision), alloc), alloc);

  rapidjson::Value catDefaults(rapidjson::kObjectType);
  for (const auto &[k, v] : doc.categoryDefaults.byCategory) {
    catDefaults.AddMember(rapidjson::Value(k.c_str(), alloc),
                          rapidjson::Value(decisionToWire(v), alloc), alloc);
  }
  d.AddMember("category_defaults", catDefaults, alloc);

  rapidjson::Value rules(rapidjson::kArrayType);
  for (const auto &r : doc.rules) {
    if (r.scope == RuleScope::Session) continue;  // never persist session
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("id", rapidjson::Value(r.id.c_str(), alloc), alloc);
    v.AddMember("category", rapidjson::Value(r.category.c_str(), alloc), alloc);
    v.AddMember("decision",
                rapidjson::Value(decisionToWire(r.decision), alloc), alloc);
    v.AddMember("scope",
                rapidjson::Value(scopeToWire(r.scope), alloc), alloc);
    if (!r.comment.empty()) {
      v.AddMember("comment", rapidjson::Value(r.comment.c_str(), alloc), alloc);
    }
    if (r.createdAt > 0)
      v.AddMember("created_at", r.createdAt, alloc);
    if (r.expiresAt > 0)
      v.AddMember("expires_at", r.expiresAt, alloc);
    if (!r.modeId.empty())
      v.AddMember("mode_id", rapidjson::Value(r.modeId.c_str(), alloc), alloc);
    rapidjson::Value match(rapidjson::kObjectType);
    for (const auto &[mk, mv] : r.match) {
      match.AddMember(rapidjson::Value(mk.c_str(), alloc),
                      rapidjson::Value(mv.c_str(), alloc), alloc);
    }
    v.AddMember("match", match, alloc);
    rules.PushBack(v, alloc);
  }
  d.AddMember("rules", rules, alloc);

  if (!doc.activeModeId.empty()) {
    d.AddMember("active_mode_id",
                rapidjson::Value(doc.activeModeId.c_str(), alloc), alloc);
  }
  rapidjson::Value modes(rapidjson::kArrayType);
  for (const auto &m : doc.modes) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("id", rapidjson::Value(m.id.c_str(), alloc), alloc);
    v.AddMember("name", rapidjson::Value(m.name.c_str(), alloc), alloc);
    if (!m.description.empty()) {
      v.AddMember("description",
                  rapidjson::Value(m.description.c_str(), alloc), alloc);
    }
    v.AddMember("built_in", m.builtIn, alloc);
    rapidjson::Value cd(rapidjson::kObjectType);
    for (const auto &[cat, dec] : m.categoryDefaults.byCategory) {
      cd.AddMember(rapidjson::Value(cat.c_str(), alloc),
                   rapidjson::Value(decisionToWire(dec), alloc), alloc);
    }
    v.AddMember("category_defaults", cd, alloc);
    modes.PushBack(v, alloc);
  }
  d.AddMember("modes", modes, alloc);

  rapidjson::StringBuffer sb;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
  writer.SetIndent(' ', 2);
  d.Accept(writer);
  return std::string(sb.GetString()) + "\n";
}

} // namespace firmius::core
