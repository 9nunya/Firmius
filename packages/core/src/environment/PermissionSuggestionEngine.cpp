#include "environment/PermissionSuggestionEngine.hpp"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>

namespace firmius::core {

namespace {

// Project root markers. If we walk up from a path and find one of these,
// we use the dir containing it as a "project root" for scope suggestions.
const std::vector<std::string> kProjectMarkers = {
    ".git", ".firmius", "package.json", "Cargo.toml", "pyproject.toml",
    "go.mod", "CMakeLists.txt", "pom.xml", "build.gradle", "Gemfile",
};

/// Walk up from `path` looking for any `kProjectMarkers` entry. Returns
/// the directory containing the marker, or empty.
std::filesystem::path findProjectRoot(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::path cur = path;
  if (!std::filesystem::is_directory(cur, ec)) {
    cur = cur.parent_path();
  }
  for (int i = 0; i < 32 && !cur.empty() && cur != cur.root_path(); ++i) {
    for (const auto &marker : kProjectMarkers) {
      if (std::filesystem::exists(cur / marker, ec)) return cur;
    }
    cur = cur.parent_path();
  }
  return {};
}

/// Escape regex meta-characters so we can produce a regex that matches
/// a literal substring.
std::string escapeRegex(const std::string &s) {
  std::string out;
  out.reserve(s.size() * 2);
  for (char c : s) {
    if (c == '.' || c == '\\' || c == '+' || c == '*' || c == '?' ||
        c == '^' || c == '$' || c == '(' || c == ')' || c == '[' ||
        c == ']' || c == '{' || c == '}' || c == '|') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

PolicyRule makeRule(const std::string &category,
                    PolicyDecision decision,
                    RuleScope scope,
                    std::map<std::string, std::string> match,
                    std::string comment = "") {
  PolicyRule r;
  r.category = category;
  r.decision = decision;
  r.scope = scope;
  r.match = std::move(match);
  r.comment = std::move(comment);
  return r;
}

PermissionSuggestion makeSuggestion(std::string label,
                                    std::string explanation,
                                    PolicyRule rule,
                                    bool defaultSelected = false) {
  PermissionSuggestion s;
  s.label = std::move(label);
  s.explanation = std::move(explanation);
  s.rule = std::move(rule);
  s.defaultSelected = defaultSelected;
  return s;
}

/// Determine whether a path looks "sensitive" — used to mark deny
/// suggestions for things like ~/.ssh, ~/.aws, *.env, *secret*.
bool looksSensitive(const std::string &path) {
  static const std::vector<std::regex> patterns = {
      std::regex(R"((^|/)\.ssh(/|$))"),
      std::regex(R"((^|/)\.aws(/|$))"),
      std::regex(R"((^|/)\.gnupg(/|$))"),
      std::regex(R"((^|/)id_(rsa|ed25519|ecdsa)(\.pub)?$)"),
      std::regex(R"(\.env(\..+)?$)"),
      std::regex(R"((^|/)secrets?(/|\.|$))", std::regex::icase),
      std::regex(R"((^|/)credentials?(/|\.|$))", std::regex::icase),
  };
  for (const auto &p : patterns) {
    if (std::regex_search(path, p)) return true;
  }
  return false;
}

} // namespace

std::vector<PermissionSuggestion> PermissionSuggestionEngine::generate(
    const PolicyRequest &request, const shared::CommandIntent &intent) {
  if (request.category == kCatProcessExec) return forProcessExec(request, intent);
  if (request.category == kCatProcessCwd)  return forProcessCwd(request);
  if (request.category == kCatFileRead)    return forFileRead(request);
  if (request.category == kCatFileWrite)   return forFileWrite(request);
  if (request.category == kCatFileCreate)  return forFileCreate(request);
  if (request.category == kCatFileDelete)  return forFileDelete(request);
  if (request.category == kCatNetworkFetch) return forNetworkFetch(request);
  if (request.category == kCatNetworkSearch) return forNetworkSearch(request);
  if (request.category == kCatAgentSpawn)  return forAgentSpawn(request);
  return {};
}

// ── process.exec ───────────────────────────────────────────────────────

std::vector<PermissionSuggestion>
PermissionSuggestionEngine::forProcessExec(const PolicyRequest &request,
                                            const shared::CommandIntent &intent) {
  std::vector<PermissionSuggestion> out;

  // Suggestion 1: per-subcommand programs. Walk parsedCommands, take
  // the first token of each, dedupe, and offer "allow `<program> *`"
  // for each one.
  std::set<std::string> seenPrograms;
  for (const auto &sub : intent.parsedCommands) {
    std::string prog;
    for (char c : sub) {
      if (c == ' ' || c == '\t') break;
      prog.push_back(c);
    }
    if (prog.empty()) continue;
    if (!seenPrograms.insert(prog).second) continue;
    // Skip shell builtins / pipes / operators.
    if (prog == "&&" || prog == "||" || prog == ";" || prog == "|") continue;

    const std::string regex = "^" + escapeRegex(prog) + "(\\s|$)";
    auto rule = makeRule(kCatProcessExec, PolicyDecision::Allow,
                         RuleScope::Global,
                         {{"command_regex", regex}});
    out.push_back(makeSuggestion(
        "Allow `" + prog + "` family",
        "matches " + regex,
        std::move(rule),
        seenPrograms.size() == 1));  // first one default-checked
  }

  // Suggestion: allow exact full command, globally.
  if (!request.command.empty()) {
    const std::string regex = "^" + escapeRegex(request.command) + "$";
    auto rule = makeRule(kCatProcessExec, PolicyDecision::Allow,
                         RuleScope::Global,
                         {{"command_regex", regex}});
    out.push_back(makeSuggestion(
        "Allow exactly this command",
        "matches: " + request.command,
        std::move(rule)));
  }

  // Suggestion: allow Process tool for the rest of this session.
  {
    auto rule = makeRule(kCatProcessExec, PolicyDecision::Allow,
                         RuleScope::Session,
                         {{"tool", "Process"}});
    out.push_back(makeSuggestion(
        "Allow Process tool, this session only",
        "auto-approve every command from this Process tool until restart",
        std::move(rule)));
  }

  // Suggestion: allow current cwd. If we can find a project root above,
  // suggest the broader scope as a separate option.
  if (!request.cwd.empty()) {
    {
      auto rule = makeRule(kCatProcessExec, PolicyDecision::Allow,
                           RuleScope::Global,
                           {{"cwd_glob", request.cwd + "/**"}});
      out.push_back(makeSuggestion(
          "Allow any command from cwd " + request.cwd,
          "matches cwd_glob: " + request.cwd + "/**",
          std::move(rule)));
    }
    auto root = findProjectRoot(std::filesystem::path(request.cwd));
    if (!root.empty() && root.string() != request.cwd) {
      auto rule = makeRule(kCatProcessExec, PolicyDecision::Allow,
                           RuleScope::Global,
                           {{"cwd_glob", root.string() + "/**"}});
      out.push_back(makeSuggestion(
          "Allow any command in project " + root.filename().string(),
          "matches cwd_glob: " + root.string() + "/**",
          std::move(rule)));
    }
  }

  return out;
}

// ── process.cwd ────────────────────────────────────────────────────────

std::vector<PermissionSuggestion>
PermissionSuggestionEngine::forProcessCwd(const PolicyRequest &request) {
  std::vector<PermissionSuggestion> out;
  if (request.cwd.empty()) return out;
  out.push_back(makeSuggestion(
      "Allow this exact cwd",
      "matches cwd_glob: " + request.cwd,
      makeRule(kCatProcessCwd, PolicyDecision::Allow, RuleScope::Global,
               {{"cwd_glob", request.cwd}}),
      true));
  out.push_back(makeSuggestion(
      "Allow this cwd subtree",
      "matches cwd_glob: " + request.cwd + "/**",
      makeRule(kCatProcessCwd, PolicyDecision::Allow, RuleScope::Global,
               {{"cwd_glob", request.cwd + "/**"}})));
  auto root = findProjectRoot(std::filesystem::path(request.cwd));
  if (!root.empty() && root.string() != request.cwd) {
    out.push_back(makeSuggestion(
        "Allow project " + root.filename().string() + " (and subdirs)",
        "matches cwd_glob: " + root.string() + "/**",
        makeRule(kCatProcessCwd, PolicyDecision::Allow, RuleScope::Global,
                 {{"cwd_glob", root.string() + "/**"}})));
  }
  return out;
}

// ── file.* generic helpers ─────────────────────────────────────────────

namespace {

/// Push the standard "exact path / parent dir / project root / extension"
/// suggestion menu onto `out` for a given file-flavored category.
void pushFilePathSuggestions(std::vector<PermissionSuggestion> &out,
                              const std::string &category,
                              const std::string &path,
                              bool denySensitive) {
  if (path.empty()) return;
  std::filesystem::path p(path);

  // Exact path.
  out.push_back(makeSuggestion(
      "Allow this exact path",
      "matches path_glob: " + path,
      makeRule(category, PolicyDecision::Allow, RuleScope::Global,
               {{"path_glob", path}}),
      true));

  // Parent dir.
  if (p.has_parent_path()) {
    const std::string parent = p.parent_path().string();
    out.push_back(makeSuggestion(
        "Allow everything in " + parent,
        "matches path_glob: " + parent + "/**",
        makeRule(category, PolicyDecision::Allow, RuleScope::Global,
                 {{"path_glob", parent + "/**"}})));
  }

  // Project root.
  auto root = findProjectRoot(p);
  if (!root.empty() && root != p && root != p.parent_path()) {
    out.push_back(makeSuggestion(
        "Allow project " + root.filename().string(),
        "matches path_glob: " + root.string() + "/**",
        makeRule(category, PolicyDecision::Allow, RuleScope::Global,
                 {{"path_glob", root.string() + "/**"}})));
  }

  // Extension-based.
  if (p.has_extension()) {
    const std::string ext = p.extension().string();
    out.push_back(makeSuggestion(
        "Allow all `" + ext + "` files",
        "matches path_glob: **/*" + ext,
        makeRule(category, PolicyDecision::Allow, RuleScope::Global,
                 {{"path_glob", "**/*" + ext}})));
  }

  // Session-wide allow.
  out.push_back(makeSuggestion(
      "Allow this category, this session only",
      "session-scoped grant for " + category,
      makeRule(category, PolicyDecision::Allow, RuleScope::Session, {})));

  // Deny suggestion if sensitive.
  if (denySensitive && looksSensitive(path)) {
    out.push_back(makeSuggestion(
        "Block this path globally",
        "permanent deny for path_glob: " + path,
        makeRule(category, PolicyDecision::Deny, RuleScope::Global,
                 {{"path_glob", path}})));
  }
}

} // namespace

std::vector<PermissionSuggestion>
PermissionSuggestionEngine::forFileRead(const PolicyRequest &request) {
  std::vector<PermissionSuggestion> out;
  pushFilePathSuggestions(out, kCatFileRead, request.path,
                           /*denySensitive=*/true);
  return out;
}

std::vector<PermissionSuggestion>
PermissionSuggestionEngine::forFileWrite(const PolicyRequest &request) {
  std::vector<PermissionSuggestion> out;
  pushFilePathSuggestions(out, kCatFileWrite, request.path, true);
  return out;
}

std::vector<PermissionSuggestion>
PermissionSuggestionEngine::forFileCreate(const PolicyRequest &request) {
  std::vector<PermissionSuggestion> out;
  pushFilePathSuggestions(out, kCatFileCreate, request.path, true);
  return out;
}

std::vector<PermissionSuggestion>
PermissionSuggestionEngine::forFileDelete(const PolicyRequest &request) {
  std::vector<PermissionSuggestion> out;
  // Delete is high-risk: never default-select an allow.
  pushFilePathSuggestions(out, kCatFileDelete, request.path, true);
  for (auto &s : out) s.defaultSelected = false;
  return out;
}

// ── network.fetch ──────────────────────────────────────────────────────

std::vector<PermissionSuggestion>
PermissionSuggestionEngine::forNetworkFetch(const PolicyRequest &request) {
  std::vector<PermissionSuggestion> out;
  if (!request.url.empty()) {
    const std::string regex = "^" + escapeRegex(request.url) + "$";
    out.push_back(makeSuggestion(
        "Allow exactly this URL",
        "matches url_regex: " + regex,
        makeRule(kCatNetworkFetch, PolicyDecision::Allow, RuleScope::Global,
                 {{"url_regex", regex}}),
        true));
  }
  if (!request.host.empty()) {
    out.push_back(makeSuggestion(
        "Allow host " + request.host,
        "matches host_glob: " + request.host,
        makeRule(kCatNetworkFetch, PolicyDecision::Allow, RuleScope::Global,
                 {{"host_glob", request.host}})));
    // Suggest the parent domain too.
    auto firstDot = request.host.find('.');
    if (firstDot != std::string::npos &&
        firstDot != request.host.rfind('.')) {
      const std::string parentDomain = request.host.substr(firstDot + 1);
      out.push_back(makeSuggestion(
          "Allow *." + parentDomain,
          "matches host_glob: *." + parentDomain,
          makeRule(kCatNetworkFetch, PolicyDecision::Allow,
                   RuleScope::Global,
                   {{"host_glob", "*." + parentDomain}})));
    }
  }
  out.push_back(makeSuggestion(
      "Allow all network fetches, this session",
      "session-scoped grant",
      makeRule(kCatNetworkFetch, PolicyDecision::Allow, RuleScope::Session,
               {})));
  return out;
}

std::vector<PermissionSuggestion>
PermissionSuggestionEngine::forNetworkSearch(const PolicyRequest &request) {
  std::vector<PermissionSuggestion> out;
  out.push_back(makeSuggestion(
      "Allow all network searches, this session",
      "session-scoped grant",
      makeRule(kCatNetworkSearch, PolicyDecision::Allow, RuleScope::Session,
               {}),
      true));
  return out;
  (void)request;
}

// ── agent.spawn ────────────────────────────────────────────────────────

std::vector<PermissionSuggestion>
PermissionSuggestionEngine::forAgentSpawn(const PolicyRequest &request) {
  std::vector<PermissionSuggestion> out;
  if (!request.persona.empty()) {
    out.push_back(makeSuggestion(
        "Allow persona '" + request.persona + "'",
        "matches persona: " + request.persona,
        makeRule(kCatAgentSpawn, PolicyDecision::Allow, RuleScope::Global,
                 {{"persona", request.persona}}),
        true));
  }
  if (!request.parentPersona.empty() && !request.persona.empty()) {
    out.push_back(makeSuggestion(
        "Allow " + request.parentPersona + " → " + request.persona,
        "scoped delegation pair",
        makeRule(kCatAgentSpawn, PolicyDecision::Allow, RuleScope::Global,
                 {{"persona", request.persona},
                  {"parent_persona", request.parentPersona}})));
  }
  out.push_back(makeSuggestion(
      "Allow all subagent spawns, this session",
      "session-scoped grant",
      makeRule(kCatAgentSpawn, PolicyDecision::Allow, RuleScope::Session,
               {})));
  return out;
}

} // namespace firmius::core
