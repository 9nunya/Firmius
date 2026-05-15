#include "tools/FilesTool.hpp"

#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "agents/PurposeLoader.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::core {
using namespace firmius::shared;

namespace {

// Maximum file size for file_read tool (2MB default)
static constexpr std::uint64_t MAX_FILE_SIZE_BYTES = 2ULL * 1024 * 1024;

// --- Read Helpers ---

shared::ToolResult executeRead(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string path;
  if (input.HasMember("path") && input["path"].IsString()) {
    path = input["path"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: path");
  }

  int start_line = 1;
  if (input.HasMember("start_line") && input["start_line"].IsInt()) {
    start_line = input["start_line"].GetInt();
  }

  int end_line = -1;
  if (input.HasMember("end_line") && input["end_line"].IsInt()) {
    end_line = input["end_line"].GetInt();
  }

  std::string absolutePath =
      ctx.agent.getEnvironment()->getWorkspace().resolvePath(path);

  try {
    ctx.agent.getPermissions()->validatePathAccess(
        absolutePath, firmius::shared::AccessMode::READ);
    PurposeLoader::loadDiscoveredAgentsForPath(ctx.agent.getMutableContext(),
                                               absolutePath);

    // Check file size before reading to prevent bad_alloc
    std::uint64_t fileSize = 0;
    try {
      fileSize = std::filesystem::file_size(absolutePath);
    } catch (const std::exception &) {
      // If we can't determine size, proceed with read
    }

    bool truncated = false;
    std::vector<uint8_t> data;
    if (fileSize > MAX_FILE_SIZE_BYTES) {
      std::ifstream file(absolutePath, std::ios::binary);
      if (!file.is_open())
        throw std::runtime_error("Could not open file: " + absolutePath);
      data.resize(MAX_FILE_SIZE_BYTES);
      file.read(reinterpret_cast<char *>(data.data()), MAX_FILE_SIZE_BYTES);
      data.resize(static_cast<std::size_t>(file.gcount()));
      truncated = true;
    } else {
      data = ctx.host.readFile(absolutePath);
    }
    std::string content(data.begin(), data.end());
    std::stringstream ss(content);

    std::vector<std::string> selectedLines;
    std::string line;
    int current = 1;
    bool reachedEnd = false;
    int lines_taken = 0;
    while (std::getline(ss, line)) {
      if (current >= start_line &&
          (end_line == -1 || current <= end_line)) {
        lines_taken++;
        selectedLines.push_back(line);
      }
      if (end_line != -1 && current >= end_line) {
        reachedEnd = (ss.peek() == EOF);
        current++;
        break;
      }
      current++;
      if (ss.eof())
        reachedEnd = true;
    }
    if (ss.eof())
      reachedEnd = true;

    rapidjson::Document res;
    res.SetObject();
    auto &alloc = res.GetAllocator();
    int normalized_start = std::max(1, start_line);
    int normalized_end = normalized_start + lines_taken - 1;
    if (lines_taken == 0)
      normalized_end = normalized_start - 1;
    bool read_full = reachedEnd && start_line <= 1;

    if (read_full) {
      ctx.agent.getEnvironment()->getWorkspace().markFileAsFullyRead(
          absolutePath);
    } else {
      ctx.agent.getEnvironment()->getWorkspace().recordFileRead(
          absolutePath, normalized_start, normalized_end, reachedEnd);
    }

    std::string enhancedContent;
    if (!selectedLines.empty()) {
      for (std::size_t i = 0; i < selectedLines.size(); ++i) {
        enhancedContent += std::to_string(start_line + static_cast<int>(i));
        enhancedContent += '|';
        enhancedContent += selectedLines[i];
        if (i + 1 < selectedLines.size()) {
          enhancedContent += '\n';
        }
      }
    }

    res.AddMember("content",
                  rapidjson::Value(enhancedContent.c_str(), alloc).Move(),
                  alloc);
    res.AddMember("line_start", normalized_start, alloc);
    res.AddMember("line_end", normalized_end, alloc);
    res.AddMember("lines_read", lines_taken, alloc);
    res.AddMember("read_full", read_full, alloc);
    res.AddMember("reached_end", reachedEnd, alloc);
    res.AddMember("watch_scope",
                  rapidjson::Value(read_full ? "full" : "range", alloc).Move(),
                  alloc);
    res.AddMember("watch_state", rapidjson::Value("updated", alloc).Move(),
                  alloc);
    if (truncated) {
      res.AddMember("truncated", true, alloc);
      std::string warning = "File content truncated: file size (" +
                            std::to_string(fileSize) + " bytes) exceeds limit of " +
                            std::to_string(MAX_FILE_SIZE_BYTES) + " bytes.";
      res.AddMember("warning",
                    rapidjson::Value(warning.c_str(), alloc).Move(), alloc);
    }
    return shared::ToolResult::ok(res);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

// --- List Helpers ---

shared::ToolResult executeList(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string path;
  if (input.HasMember("path") && input["path"].IsString()) {
    path = input["path"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: path");
  }

  bool include_hidden = false;
  if (input.HasMember("include_hidden") && input["include_hidden"].IsBool()) {
    include_hidden = input["include_hidden"].GetBool();
  }

  try {
    std::string absPath = ctx.agent.getEnvironment()->getWorkspace().resolvePath(path);
    ctx.agent.getPermissions()->validatePathAccess(absPath, firmius::shared::AccessMode::READ);

    auto entries = ctx.host.listDir(absPath);

    rapidjson::Document doc;
    doc.SetArray();
    auto &a = doc.GetAllocator();

    for (const auto &entry : entries) {
      if (!include_hidden && !entry.name.empty() && entry.name[0] == '.') {
        continue;
      }
      rapidjson::Value val(rapidjson::kObjectType);
      val.AddMember("name", rapidjson::Value(entry.name.c_str(), a).Move(), a);
      val.AddMember("path", rapidjson::Value(entry.path.c_str(), a).Move(), a);
      val.AddMember("size", entry.size, a);
      val.AddMember("is_directory", entry.isDirectory, a);
      val.AddMember("is_symlink", entry.isSymlink, a);
      val.AddMember("modified_ms", entry.modifiedMs, a);
      doc.PushBack(val, a);
    }

    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

// --- Grep Helpers ---

bool commandLooksUnavailable(const ProcessResult &result) {
  if (result.exitCode == 127) {
    return true;
  }
  return result.stderrData.find("not found") != std::string::npos ||
         result.stderrData.find("No such file or directory") != std::string::npos;
}

std::string buildRipgrepCommand(const std::string &pattern, int before, int after, const std::string &absPath) {
  std::string command = "rg --json --pcre2 --line-number --with-filename -e " +
                        shared::StringUtil::shellEscape(pattern);
  if (before > 0) command += " -B " + std::to_string(before);
  if (after > 0) command += " -A " + std::to_string(after);
  command += " " + shared::StringUtil::shellEscape(absPath);
  return command;
}

std::string buildGrepCommand(const std::string &pattern, int before, int after, const std::string &absPath, bool perlMode) {
  std::string command = "grep -rnH";
  command += perlMode ? "P" : "E";
  command += " -e " + shared::StringUtil::shellEscape(pattern);
  if (before > 0) command += " -B " + std::to_string(before);
  if (after > 0) command += " -A " + std::to_string(after);
  command += " " + shared::StringUtil::shellEscape(absPath);
  return command;
}

bool appendRipgrepJsonLine(const std::string &line, rapidjson::Value &results, rapidjson::Document::AllocatorType &alloc) {
  if (line.empty()) return true;
  rapidjson::Document event;
  event.Parse(line.c_str());
  if (event.HasParseError() || !event.IsObject() || !event.HasMember("type") ||
      !event["type"].IsString() || !event.HasMember("data") || !event["data"].IsObject()) {
    return false;
  }
  const std::string type = event["type"].GetString();
  if (type != "match" && type != "context") return true;
  const auto &data = event["data"];
  if (!data.HasMember("path") || !data["path"].IsObject() || !data.HasMember("lines") ||
      !data["lines"].IsObject() || !data.HasMember("line_number") || !data["line_number"].IsInt()) {
    return false;
  }
  const auto &path = data["path"];
  const auto &lines = data["lines"];
  if (!path.HasMember("text") || !path["text"].IsString() || !lines.HasMember("text") || !lines["text"].IsString()) {
    return false;
  }
  rapidjson::Value value(rapidjson::kObjectType);
  value.AddMember("file", rapidjson::Value(path["text"].GetString(), alloc).Move(), alloc);
  value.AddMember("line", data["line_number"].GetInt(), alloc);
  std::string content = lines["text"].GetString();
  if (!content.empty() && content.back() == '\n') content.pop_back();
  value.AddMember("content", rapidjson::Value(content.c_str(), alloc).Move(), alloc);
  value.AddMember("is_match", type == "match", alloc);
  results.PushBack(value, alloc);
  return true;
}

bool parseRipgrepOutput(const std::string &stdoutData, rapidjson::Value &results, rapidjson::Document::AllocatorType &alloc, bool &budgetHit) {
  std::istringstream stream(stdoutData);
  std::string line;
  while (std::getline(stream, line)) {
    if (results.Size() >= 2000) {
      budgetHit = true;
      return true;
    }
    if (!appendRipgrepJsonLine(line, results, alloc)) return false;
  }
  return true;
}

bool parseGrepOutput(const std::string &stdoutData, rapidjson::Value &results, rapidjson::Document::AllocatorType &alloc, bool &budgetHit) {
  std::istringstream stream(stdoutData);
  std::string line;
  static const std::regex matchRegex(R"(:([0-9]+):)");
  static const std::regex contextRegex(R"(-([0-9]+)-)");
  while (std::getline(stream, line)) {
    if (results.Size() >= 2000) {
      budgetHit = true;
      return true;
    }
    if (line.empty() || line == "--") continue;
    std::smatch match;
    bool isMatch = false;
    size_t separatorPosition = std::string::npos;
    size_t separatorLength = 0;
    std::string lineNumberText;
    if (std::regex_search(line, match, matchRegex)) {
      isMatch = true;
      separatorPosition = match.position();
      separatorLength = match.length();
      lineNumberText = match[1].str();
    } else if (std::regex_search(line, match, contextRegex)) {
      separatorPosition = match.position();
      separatorLength = match.length();
      lineNumberText = match[1].str();
    }
    if (separatorPosition == std::string::npos) continue;
    int lineNumber = 0;
    try { lineNumber = std::stoi(lineNumberText); } catch (...) { continue; }
    const std::string file = line.substr(0, separatorPosition);
    const std::string content = line.substr(separatorPosition + separatorLength);
    rapidjson::Value value(rapidjson::kObjectType);
    value.AddMember("file", rapidjson::Value(file.c_str(), alloc).Move(), alloc);
    value.AddMember("line", lineNumber, alloc);
    value.AddMember("content", rapidjson::Value(content.c_str(), alloc).Move(), alloc);
    value.AddMember("is_match", isMatch, alloc);
    results.PushBack(value, alloc);
  }
  return true;
}

shared::ToolResult executeGrep(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string path;
  if (input.HasMember("path") && input["path"].IsString()) {
    path = input["path"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: path");
  }

  std::string pattern;
  if (input.HasMember("pattern") && input["pattern"].IsString()) {
    pattern = input["pattern"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: pattern");
  }

  int before = 0;
  if (input.HasMember("context_before") && input["context_before"].IsInt()) {
    before = input["context_before"].GetInt();
  }
  int after = 0;
  if (input.HasMember("context_after") && input["context_after"].IsInt()) {
    after = input["context_after"].GetInt();
  }

  try {
    std::string absPath = ctx.agent.getEnvironment()->getWorkspace().resolvePath(path);
    ctx.agent.getPermissions()->validatePathAccess(absPath, firmius::shared::AccessMode::READ);

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    rapidjson::Value results(rapidjson::kArrayType);
    bool budgetHit = false;

    auto ripgrepResult = ctx.host.exec(buildRipgrepCommand(pattern, before, after, absPath), "", {}, std::chrono::milliseconds(10000));
    if (ripgrepResult.finishReason == shared::ProcessFinishReason::Timeout) {
      return shared::ToolResult::fail("Grep failed: command timed out after 10 seconds");
    }

    if (ripgrepResult.exitCode == 0) {
      if (!parseRipgrepOutput(ripgrepResult.stdoutData, results, alloc, budgetHit)) {
        return shared::ToolResult::fail("Grep failed: malformed ripgrep JSON output");
      }
      doc.AddMember("results", results, alloc);
      doc.AddMember("budget_hit", budgetHit, alloc);
      return shared::ToolResult::ok(doc);
    }

    if (ripgrepResult.exitCode == 1) {
      doc.AddMember("results", results, alloc);
      doc.AddMember("budget_hit", false, alloc);
      return shared::ToolResult::ok(doc);
    }

    ProcessResult fallbackResult;
    if (commandLooksUnavailable(ripgrepResult)) {
      fallbackResult = ctx.host.exec(buildGrepCommand(pattern, before, after, absPath, true), "", {}, std::chrono::milliseconds(10000));
      if (fallbackResult.finishReason == shared::ProcessFinishReason::Timeout) {
        return shared::ToolResult::fail("Grep failed: command timed out after 10 seconds");
      }
      if (fallbackResult.exitCode != 0 && fallbackResult.exitCode != 1 &&
          fallbackResult.stderrData.find("support for the -P option") != std::string::npos) {
        fallbackResult = ctx.host.exec(buildGrepCommand(pattern, before, after, absPath, false), "", {}, std::chrono::milliseconds(10000));
        if (fallbackResult.finishReason == shared::ProcessFinishReason::Timeout) {
          return shared::ToolResult::fail("Grep failed: command timed out after 10 seconds");
        }
      }
    } else {
      fallbackResult = ripgrepResult;
    }

    if (fallbackResult.exitCode != 0 && fallbackResult.exitCode != 1) {
      return shared::ToolResult::fail("Grep failed: " + fallbackResult.stderrData);
    }

    if (fallbackResult.exitCode == 0) {
      parseGrepOutput(fallbackResult.stdoutData, results, alloc, budgetHit);
    }

    doc.AddMember("results", results, alloc);
    doc.AddMember("budget_hit", budgetHit, alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

// --- Glob Helpers ---

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

shared::ToolResult executeGlob(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string path;
  if (input.HasMember("path") && input["path"].IsString()) {
    path = input["path"].GetString();
  } else {
    // Default to workspace root if path omitted for glob?
    // Old GlobTool requires "path" through GlobInput mapping, so we'll enforce it.
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

} // namespace

shared::ToolMetadata FilesTool::getMetadata() const {
  return {
      "Files",
      R"(Filesystem operations: read files, list directories, grep text, and expand globs.

USAGE GUIDANCE (read this):
- Prefer this tool over guessing repository state.
- Always pass paths relative to the workspace root when possible.
- Use Read before making edits so you anchor your changes to exact file contents.
- Use List/Glob to discover files; use Grep to locate symbols/strings across the repo.
- Treat returned output as authoritative: if it says budget_hit=true or truncated=true, refine the query (narrow path/glob, tighten pattern, use start_line/end_line).

ACTIONS:
- Read: Read a text file (optionally line-ranged). Returns numbered lines.
- List: List directory entries.
- Grep: Regex search through files under a directory tree.
- Glob: Expand a glob pattern (e.g. "src/**/*.cpp").

SECURITY / PERMISSIONS:
- All actions require filesystem READ access to the resolved paths.
- Binary/huge files may be truncated for safety.
)",
      shared::ToolScope::FilesystemRead};
}

std::shared_ptr<shared::JSONSchema> FilesTool::getSchema() const {
  return shared::zObject({
      {"action",
       shared::zEnum({"Read", "List", "Grep", "Glob"})
           ->describe(
               "Which filesystem operation to execute.\n\n"
               "Allowed values:\n"
               "- Read: read a file (optionally line-ranged)\n"
               "- List: list directory entries\n"
               "- Grep: search file contents for a regex pattern\n"
               "- Glob: expand a glob pattern into matching paths\n\n"
               "Usage notes: Choose the narrowest action that answers the question. "
               "If you need exact file contents, use Read; if you need discovery, use List/Glob; "
               "if you need to locate a string/symbol, use Grep.")},

      {"path",
       shared::zString()
           ->setOptional()
           ->describe(
               "Path meaning depends on action. Provide a workspace-relative path when possible.\n\n"
               "- Read: the file to read\n"
               "- List: the directory to list\n"
               "- Grep: the directory root to search under (use '.' for repo root)\n"
               "- Glob: optional base directory for resolving the glob; if omitted, the glob is evaluated from workspace root\n\n"
               "Security: must be readable under current permissions. Relative paths are resolved against the workspace root.")},

      {"pattern",
       shared::zString()
           ->setOptional()
           ->describe(
               "Regex pattern for Grep action.\n\n"
               "Required when action=Grep. Interpreted as a regular expression. "
               "If you want a literal match, escape regex metacharacters (e.g. '\\.' for '.').")},

      {"glob",
       shared::zString()
           ->setOptional()
           ->describe(
               "Glob pattern for Glob action (e.g. 'src/**/*.cpp').\n\n"
               "Required when action=Glob. Use this to discover matching paths without reading file contents.")},

      {"include_hidden",
       shared::zBoolean()
           ->setOptional()
           ->describe(
               "Whether to include dotfiles/directories (names starting with '.').\n\n"
               "Default: false. Applies primarily to List/Grep/Glob traversal.")},

      {"start_line",
       shared::zInteger()
           ->setOptional()
           ->describe(
               "For Read action: 1-based starting line to include.\n\n"
               "Default: 1. If provided with end_line, returns only that line range.")},

      {"end_line",
       shared::zInteger()
           ->setOptional()
           ->describe(
               "For Read action: 1-based ending line to include (inclusive).\n\n"
               "Default: -1 (read to end of file).")},

      {"context_before",
       shared::zInteger()
           ->setOptional()
           ->describe(
               "For Grep action: number of context lines to include before each match.\n\n"
               "Default: 0. Increase for better local reasoning around a match.")},

      {"context_after",
       shared::zInteger()
           ->setOptional()
           ->describe(
               "For Grep action: number of context lines to include after each match.\n\n"
               "Default: 0.")},
  });
}

shared::ToolResult FilesTool::execute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  if (!input.IsObject() || !input.HasMember("action") || !input["action"].IsString()) {
    return shared::ToolResult::fail("Files.action must be a string (Read, List, Grep, or Glob)");
  }
  const std::string action = input["action"].GetString();
  if (action == "Read") return executeRead(input, ctx);
  if (action == "List") return executeList(input, ctx);
  if (action == "Grep") return executeGrep(input, ctx);
  if (action == "Glob") return executeGlob(input, ctx);
  return shared::ToolResult::fail("Files.action must be Read, List, Grep, or Glob");
}

} // namespace firmius::core
