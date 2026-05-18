#include "tools/GrepTool.hpp"

#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/StringUtil.hpp"
#include <chrono>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

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

// Token-waste pass 3: a single grep hit. We collect into this lightweight
// struct and format prose at the end; we never construct a per-hit JSON
// object on the wire.
struct GrepHit {
  std::string file;
  int line = 0;
  std::string content;
  bool is_match = false;  // true = matched line, false = context line
};

bool appendRipgrepJsonLine(const std::string &line, std::vector<GrepHit> &hits) {
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
  GrepHit hit;
  hit.file = path["text"].GetString();
  hit.line = data["line_number"].GetInt();
  hit.content = lines["text"].GetString();
  if (!hit.content.empty() && hit.content.back() == '\n') hit.content.pop_back();
  hit.is_match = (type == "match");
  hits.push_back(std::move(hit));
  return true;
}

// Token-waste pass 3: cap raised from 2000 to 5000. The prose-first form
// is roughly 4x more compact per hit than the old structured array.
constexpr std::size_t kGrepHitBudget = 5000;

bool parseRipgrepOutput(const std::string &stdoutData,
                        std::vector<GrepHit> &hits, bool &budgetHit) {
  std::istringstream stream(stdoutData);
  std::string line;
  while (std::getline(stream, line)) {
    if (hits.size() >= kGrepHitBudget) {
      budgetHit = true;
      return true;
    }
    if (!appendRipgrepJsonLine(line, hits)) return false;
  }
  return true;
}

bool parseGrepOutput(const std::string &stdoutData,
                     std::vector<GrepHit> &hits, bool &budgetHit) {
  std::istringstream stream(stdoutData);
  std::string line;
  static const std::regex matchRegex(R"(:([0-9]+):)");
  static const std::regex contextRegex(R"(-([0-9]+)-)");
  while (std::getline(stream, line)) {
    if (hits.size() >= kGrepHitBudget) {
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
    GrepHit hit;
    hit.file = line.substr(0, separatorPosition);
    hit.line = lineNumber;
    hit.content = line.substr(separatorPosition + separatorLength);
    hit.is_match = isMatch;
    hits.push_back(std::move(hit));
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
  const bool wantsContent = (before > 0 || after > 0);

  try {
    std::string absPath = ctx.agent.getEnvironment()->getWorkspace().resolvePath(path);
    ctx.agent.getPermissions()->validatePathAccess(absPath, firmius::shared::AccessMode::READ);

    std::vector<GrepHit> hits;
    bool budgetHit = false;

    auto ripgrepResult = ctx.host.exec(buildRipgrepCommand(pattern, before, after, absPath), "", {}, std::chrono::milliseconds(10000));
    if (ripgrepResult.finishReason == shared::ProcessFinishReason::Timeout) {
      return shared::ToolResult::fail("Grep failed: command timed out after 10 seconds");
    }

    auto runFallback = [&]() -> shared::ToolResult {
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
        parseGrepOutput(fallbackResult.stdoutData, hits, budgetHit);
      }
      return shared::ToolResult::ok();  // sentinel: success, hits filled
    };

    if (ripgrepResult.exitCode == 0) {
      if (!parseRipgrepOutput(ripgrepResult.stdoutData, hits, budgetHit)) {
        return shared::ToolResult::fail("Grep failed: malformed ripgrep JSON output");
      }
    } else if (ripgrepResult.exitCode == 1) {
      // ripgrep exit 1 = "no matches"; leave hits empty.
    } else {
      auto fbResult = runFallback();
      if (!fbResult.success) return fbResult;
    }

    // Token-waste pass 3: build prose-first output. Group hits by file in
    // first-appearance order, list line numbers comma-separated. When the
    // agent asked for context (context_before/after > 0) we interleave
    // each hit on its own line with content; otherwise just file + line
    // numbers, which is dramatically more compact for the common
    // "where does this symbol appear?" query.
    //
    // Match-vs-context: in the no-content form we count `is_match`-only
    // entries per file ("(2 matches)"). Context lines are still counted
    // in the total but not enumerated since the model already knows they
    // exist around each match line.
    std::vector<std::string> fileOrder;
    std::map<std::string, std::vector<const GrepHit *>> byFile;
    int matchCount = 0;
    for (const auto &h : hits) {
      auto it = byFile.find(h.file);
      if (it == byFile.end()) {
        fileOrder.push_back(h.file);
        byFile[h.file] = {};
      }
      byFile[h.file].push_back(&h);
      if (h.is_match) ++matchCount;
    }

    std::ostringstream prose;
    if (hits.empty()) {
      prose << "Pattern '" << pattern << "' — no matches.";
    } else if (wantsContent) {
      prose << "Pattern '" << pattern << "' — " << matchCount << " match"
            << (matchCount == 1 ? "" : "es") << " across " << fileOrder.size()
            << " file" << (fileOrder.size() == 1 ? "" : "s") << ":\n";
      for (const auto &file : fileOrder) {
        for (const auto *h : byFile[file]) {
          // Use ':' to mark match line, '-' to mark context line — same
          // convention ripgrep / grep -A use in their plaintext output.
          prose << "  " << h->file << (h->is_match ? ":" : "-") << h->line
                << (h->is_match ? ":" : "-") << h->content << "\n";
        }
      }
    } else {
      prose << "Pattern '" << pattern << "' — " << matchCount << " match"
            << (matchCount == 1 ? "" : "es") << " across " << fileOrder.size()
            << " file" << (fileOrder.size() == 1 ? "" : "s") << ":\n";
      for (const auto &file : fileOrder) {
        const auto &fileHits = byFile[file];
        // Count match-only entries per file for the parenthetical.
        int fileMatchCount = 0;
        for (const auto *h : fileHits) if (h->is_match) ++fileMatchCount;
        prose << "  " << file << " (" << fileMatchCount << " match"
              << (fileMatchCount == 1 ? "" : "es") << "):";
        bool first = true;
        for (const auto *h : fileHits) {
          if (!h->is_match) continue;
          prose << (first ? " " : ", ") << h->line;
          first = false;
        }
        prose << "\n";
      }
    }
    if (budgetHit) {
      prose << "[budget_hit: result cap of " << kGrepHitBudget
            << " reached; refine the query to see more]";
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    std::string proseStr = prose.str();
    doc.AddMember(
        "result",
        rapidjson::Value(proseStr.c_str(),
                         static_cast<rapidjson::SizeType>(proseStr.size()),
                         alloc).Move(),
        alloc);
    doc.AddMember("hits", matchCount, alloc);
    doc.AddMember("files", static_cast<uint32_t>(fileOrder.size()), alloc);
    if (budgetHit) {
      doc.AddMember("budget_hit", true, alloc);
    }
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
