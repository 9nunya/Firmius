#include "tools/GrepTool.hpp"

#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/StringUtil.hpp"
#include <chrono>
#include <regex>
#include <sstream>
#include <string>

namespace firmius::core {
using namespace firmius::shared;

namespace {

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

} // namespace

shared::ToolMetadata GrepTool::getMetadata() const {
  return {
      "Grep",
      R"(Regex search through files under a directory tree.

USAGE GUIDANCE:
- Use Grep to locate symbols/strings across the repo.
- Always pass paths relative to the workspace root when possible.
- Treat returned output as authoritative: if budget_hit=true, refine the query (narrow path, tighten pattern).

SECURITY / PERMISSIONS:
- Requires filesystem READ access to the resolved paths.
)",
      shared::ToolScope::FilesystemRead};
}

std::shared_ptr<shared::JSONSchema> GrepTool::getSchema() const {
  return shared::zObject({
      {"path",
       shared::zString()
           ->describe(
               "The directory root to search under (use '.' for repo root). "
               "Security: must be readable under current permissions.")},
      {"pattern",
       shared::zString()
           ->describe(
               "Regex pattern to search for. Interpreted as a regular expression. "
               "If you want a literal match, escape regex metacharacters (e.g. '\\.' for '.').")},
      {"context_before",
       shared::zInteger()
           ->setOptional()
           ->describe(
               "Number of context lines to include before each match. Default: 0.")},
      {"context_after",
       shared::zInteger()
           ->setOptional()
           ->describe(
               "Number of context lines to include after each match. Default: 0.")},
  });
}

shared::ToolResult GrepTool::execute(const rapidjson::Value &input, shared::ToolContext &ctx) {
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

} // namespace firmius::core
