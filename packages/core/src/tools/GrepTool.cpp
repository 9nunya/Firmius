#include "tools/GrepTool.hpp"
#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include <iostream>
#include <rapidjson/document.h>
#include <regex>
#include <sstream>

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

std::string buildRipgrepCommand(const GrepInput &input,
                                const std::string &absPath) {
  std::string command = "rg --json --pcre2 --hidden --no-ignore --line-number "
                        "--with-filename --binary -e " +
                        shared::StringUtil::shellEscape(input.pattern);
  if (input.context_before > 0) {
    command += " -B " + std::to_string(input.context_before);
  }
  if (input.context_after > 0) {
    command += " -A " + std::to_string(input.context_after);
  }
  command += " " + shared::StringUtil::shellEscape(absPath);
  return command;
}

std::string buildGrepCommand(const GrepInput &input, const std::string &absPath,
                             bool perlMode) {
  std::string command = "grep -rnH";
  command += perlMode ? "P" : "E";
  command += " --binary-files=without-match -e " +
             shared::StringUtil::shellEscape(input.pattern);
  if (input.context_before > 0) {
    command += " -B " + std::to_string(input.context_before);
  }
  if (input.context_after > 0) {
    command += " -A " + std::to_string(input.context_after);
  }
  command += " " + shared::StringUtil::shellEscape(absPath);
  return command;
}

bool appendRipgrepJsonLine(const std::string &line, rapidjson::Document &doc) {
  if (line.empty()) {
    return true;
  }

  rapidjson::Document event;
  event.Parse(line.c_str());
  if (event.HasParseError() || !event.IsObject() || !event.HasMember("type") ||
      !event["type"].IsString() || !event.HasMember("data") ||
      !event["data"].IsObject()) {
    return false;
  }

  const std::string type = event["type"].GetString();
  if (type != "match" && type != "context") {
    return true;
  }

  const auto &data = event["data"];
  if (!data.HasMember("path") || !data["path"].IsObject() ||
      !data.HasMember("lines") || !data["lines"].IsObject() ||
      !data.HasMember("line_number") || !data["line_number"].IsInt()) {
    return false;
  }

  const auto &path = data["path"];
  const auto &lines = data["lines"];
  if (!path.HasMember("text") || !path["text"].IsString() ||
      !lines.HasMember("text") || !lines["text"].IsString()) {
    return false;
  }

  auto &alloc = doc.GetAllocator();
  rapidjson::Value value(rapidjson::kObjectType);
  value.AddMember("file",
                  rapidjson::Value(path["text"].GetString(), alloc).Move(), alloc);
  value.AddMember("line", data["line_number"].GetInt(), alloc);

  std::string content = lines["text"].GetString();
  if (!content.empty() && content.back() == '\n') {
    content.pop_back();
  }
  value.AddMember("content", rapidjson::Value(content.c_str(), alloc).Move(),
                  alloc);
  value.AddMember("is_match", type == "match", alloc);
  doc.PushBack(value, alloc);
  return true;
}

bool parseRipgrepOutput(const std::string &stdoutData, rapidjson::Document &doc) {
  std::istringstream stream(stdoutData);
  std::string line;
  while (std::getline(stream, line)) {
    if (!appendRipgrepJsonLine(line, doc)) {
      return false;
    }
  }
  return true;
}

bool parseGrepOutput(const std::string &stdoutData, rapidjson::Document &doc) {
  std::istringstream stream(stdoutData);
  std::string line;
  static const std::regex matchRegex(R"(:([0-9]+):)");
  static const std::regex contextRegex(R"(-([0-9]+)-)");

  auto &alloc = doc.GetAllocator();
  while (std::getline(stream, line)) {
    if (line.empty() || line == "--") {
      continue;
    }

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

    if (separatorPosition == std::string::npos) {
      continue;
    }

    int lineNumber = 0;
    try {
      lineNumber = std::stoi(lineNumberText);
    } catch (...) {
      continue;
    }

    const std::string file = line.substr(0, separatorPosition);
    const std::string content = line.substr(separatorPosition + separatorLength);
    rapidjson::Value value(rapidjson::kObjectType);
    value.AddMember("file", rapidjson::Value(file.c_str(), alloc).Move(), alloc);
    value.AddMember("line", lineNumber, alloc);
    value.AddMember("content",
                    rapidjson::Value(content.c_str(), alloc).Move(), alloc);
    value.AddMember("is_match", isMatch, alloc);
    doc.PushBack(value, alloc);
  }

  return true;
}

} // namespace

shared::ToolResult GrepTool::execute(const GrepInput &input,
                                     shared::ToolContext &ctx) {
  try {
    std::string absPath = ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.path);

    ctx.agent.getPermissions()->validatePathAccess(absPath, firmius::shared::AccessMode::READ);

    rapidjson::Document doc;
    doc.SetArray();

    auto ripgrepResult = ctx.host.exec(buildRipgrepCommand(input, absPath));
    if (ripgrepResult.exitCode == 0) {
      if (!parseRipgrepOutput(ripgrepResult.stdoutData, doc)) {
        return shared::ToolResult::fail("Grep failed: malformed ripgrep JSON output");
      }
      return shared::ToolResult::ok(doc);
    }

    if (ripgrepResult.exitCode == 1) {
      return shared::ToolResult::ok(doc);
    }

    ProcessResult fallbackResult;
    if (commandLooksUnavailable(ripgrepResult)) {
      fallbackResult = ctx.host.exec(buildGrepCommand(input, absPath, true));
      if (fallbackResult.exitCode != 0 && fallbackResult.exitCode != 1 &&
          fallbackResult.stderrData.find("support for the -P option") !=
              std::string::npos) {
        fallbackResult = ctx.host.exec(buildGrepCommand(input, absPath, false));
      }
    } else {
      fallbackResult = ripgrepResult;
    }

    if (fallbackResult.exitCode != 0 && fallbackResult.exitCode != 1) {
      return shared::ToolResult::fail("Grep failed: " + fallbackResult.stderrData);
    }

    if (fallbackResult.exitCode == 0) {
      parseGrepOutput(fallbackResult.stdoutData, doc);
    }

    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
