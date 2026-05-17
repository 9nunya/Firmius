#include "tools/GlobTool.hpp"

#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include <algorithm>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace firmius::core {
using namespace firmius::shared;

namespace {

std::string normalizeSlashes(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  return value;
}

bool isRegexMeta(char ch) {
  switch (ch) {
  case '.': case '^': case '$': case '|': case '(': case ')':
  case '[': case ']': case '{': case '}': case '+': case '?':
  case '*': case '\\': return true;
  default: return false;
  }
}

std::vector<std::string> splitBraceAlternatives(const std::string &body) {
  std::vector<std::string> parts;
  std::string current;
  int depth = 0;
  for (size_t i = 0; i < body.size(); ++i) {
    const char ch = body[i];
    if (ch == '\\' && i + 1 < body.size()) {
      current.push_back(ch);
      current.push_back(body[++i]);
      continue;
    }
    if (ch == '{') { ++depth; current.push_back(ch); continue; }
    if (ch == '}') { --depth; current.push_back(ch); continue; }
    if (ch == ',' && depth == 0) {
      parts.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  parts.push_back(current);
  return parts;
}

std::string globPatternToRegexBody(const std::string &pattern);

std::optional<std::pair<size_t, std::string>> parseBraceExpression(const std::string &pattern, size_t start) {
  int depth = 0;
  std::string body;
  for (size_t i = start; i < pattern.size(); ++i) {
    const char ch = pattern[i];
    if (ch == '\\' && i + 1 < pattern.size()) {
      body.push_back(ch);
      body.push_back(pattern[++i]);
      continue;
    }
    if (ch == '{') {
      ++depth;
      if (depth > 1) body.push_back(ch);
      continue;
    }
    if (ch == '}') {
      --depth;
      if (depth == 0) return std::make_pair(i, body);
      if (depth < 0) return std::nullopt;
      body.push_back(ch);
      continue;
    }
    body.push_back(ch);
  }
  return std::nullopt;
}

std::string globPatternToRegexBody(const std::string &pattern) {
  std::string regex;
  for (size_t i = 0; i < pattern.size(); ++i) {
    const char ch = pattern[i];
    if (ch == '\\' && i + 1 < pattern.size()) {
      const char literal = pattern[++i];
      if (isRegexMeta(literal)) regex.push_back('\\');
      regex.push_back(literal);
      continue;
    }
    if (ch == '*') {
      const bool doubleStar = (i + 1 < pattern.size() && pattern[i + 1] == '*');
      if (doubleStar) {
        while (i + 1 < pattern.size() && pattern[i + 1] == '*') ++i;
        if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
          ++i;
          regex += "(?:.*/)?";
        } else {
          regex += ".*";
        }
      } else {
        regex += "[^/]*";
      }
      continue;
    }
    if (ch == '?') { regex += "[^/]"; continue; }
    if (ch == '[') {
      size_t close = i + 1;
      while (close < pattern.size() && pattern[close] != ']') {
        if (pattern[close] == '\\' && close + 1 < pattern.size()) close += 2;
        else ++close;
      }
      if (close >= pattern.size()) { regex += "\\["; continue; }
      std::string charClass = pattern.substr(i + 1, close - i - 1);
      if (!charClass.empty() && (charClass.front() == '!' || charClass.front() == '^')) {
        charClass.front() = '^';
      }
      regex += "["; regex += charClass; regex += "]";
      i = close;
      continue;
    }
    if (ch == '{') {
      auto brace = parseBraceExpression(pattern, i);
      if (brace.has_value()) {
        const auto alternatives = splitBraceAlternatives(brace->second);
        regex += "(?:";
        for (size_t idx = 0; idx < alternatives.size(); ++idx) {
          if (idx > 0) regex += "|";
          regex += globPatternToRegexBody(alternatives[idx]);
        }
        regex += ")";
        i = brace->first;
        continue;
      }
    }
    if (isRegexMeta(ch)) regex.push_back('\\');
    regex.push_back(ch);
  }
  return regex;
}

std::regex compileGlobRegex(const std::string &pattern) {
  return std::regex("^" + globPatternToRegexBody(normalizeSlashes(pattern)) + "$");
}

bool matchesGlobPattern(const std::string &pattern, const std::string &relativePath, const std::string &basename) {
  const std::regex fullRegex = compileGlobRegex(pattern);
  if (std::regex_match(relativePath, fullRegex)) return true;
  if (pattern.find('/') == std::string::npos) return std::regex_match(basename, fullRegex);
  return false;
}

std::string lexicalRelativePath(const std::string &path, const std::string &root) {
  std::filesystem::path normalizedPath(path);
  std::filesystem::path normalizedRoot(root);
  auto relative = normalizedPath.lexically_relative(normalizedRoot);
  if (relative.empty()) return normalizeSlashes(normalizedPath.filename().generic_string());
  return normalizeSlashes(relative.generic_string());
}

constexpr size_t kMaxGlobVisitedNodes = 20000;
constexpr size_t kMaxGlobMatches = 1000;

void collectGlobMatches(shared::IHost &host, const std::string &rootPath, const std::string &currentPath, const std::string &pattern,
                        std::vector<std::string> &matches, size_t &visitedNodes) {
  if (visitedNodes >= kMaxGlobVisitedNodes || matches.size() >= kMaxGlobMatches) return;
  ++visitedNodes;
  const auto info = host.stat(currentPath);
  const std::string relativePath = lexicalRelativePath(currentPath, rootPath);
  const std::string basename = normalizeSlashes(info.name);
  if (!relativePath.empty() && matchesGlobPattern(pattern, relativePath, basename)) {
    matches.push_back(currentPath);
  }
  if (!info.isDirectory || info.isSymlink) return;
  auto entries = host.listDir(currentPath);
  std::sort(entries.begin(), entries.end(), [](const FileInfo &lhs, const FileInfo &rhs) { return lhs.path < rhs.path; });
  for (const auto &entry : entries) {
    collectGlobMatches(host, rootPath, entry.path, pattern, matches, visitedNodes);
  }
}

} // namespace

shared::ToolMetadata GlobTool::getMetadata() const {
  return {
      "Glob",
      R"(Expand a glob pattern into matching paths (e.g. "src/**/*.cpp").

USAGE GUIDANCE:
- Use Glob to discover matching paths without reading file contents.
- Always pass paths relative to the workspace root when possible.
- Treat returned output as authoritative: if budget_hit=true, refine the glob.

SECURITY / PERMISSIONS:
- Requires filesystem READ access to the resolved paths.
)",
      shared::ToolScope::FilesystemRead};
}

std::shared_ptr<shared::JSONSchema> GlobTool::getSchema() const {
  return shared::zObject({
      {"path",
       shared::zString()
           ->describe(
               "Base directory for resolving the glob. Provide a workspace-relative path when possible. "
               "Security: must be readable under current permissions.")},
      {"glob",
       shared::zString()
           ->describe(
               "Glob pattern (e.g. 'src/**/*.cpp'). Use this to discover matching paths without reading file contents.")},
  });
}

shared::ToolResult GlobTool::execute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string path;
  if (input.HasMember("path") && input["path"].IsString()) {
    path = input["path"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: path");
  }

  std::string pattern;
  if (input.HasMember("glob") && input["glob"].IsString()) {
    pattern = input["glob"].GetString();
  } else if (input.HasMember("pattern") && input["pattern"].IsString()) {
    pattern = input["pattern"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: glob");
  }

  try {
    std::string absPath = ctx.agent.getEnvironment()->getWorkspace().resolvePath(path);
    ctx.agent.getPermissions()->validatePathAccess(absPath, firmius::shared::AccessMode::READ);

    std::vector<std::string> matches;
    size_t visitedNodes = 0;
    collectGlobMatches(ctx.host, absPath, absPath, pattern, matches, visitedNodes);
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());

    bool budgetHit = visitedNodes >= kMaxGlobVisitedNodes || matches.size() >= kMaxGlobMatches;

    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();
    rapidjson::Value results(rapidjson::kArrayType);
    for (const auto &match : matches) {
      results.PushBack(rapidjson::Value(match.c_str(), a).Move(), a);
    }
    doc.AddMember("results", results, a);
    doc.AddMember("budget_hit", budgetHit, a);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
