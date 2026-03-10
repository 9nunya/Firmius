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

shared::ToolResult GrepTool::execute(const GrepInput &input,
                                     shared::ToolContext &ctx) {
  try {
    std::string absPath = ctx.agent.resolvePath(input.path);

    ctx.agent.getPermissionChecks().validatePathAccess(absPath);

    std::string command = "grep -rnHE --binary-files=without-match " +
                          shared::StringUtil::shellEscape(input.pattern);
    if (input.context_before > 0)
      command += " -B " + std::to_string(input.context_before);
    if (input.context_after > 0)
      command += " -A " + std::to_string(input.context_after);
    command += " " + shared::StringUtil::shellEscape(absPath);

    auto res = ctx.host.exec(command);

    // Handle exit code 1 (no matches) as success with empty results
    if (res.exitCode != 0 && res.exitCode != 1) {
      return shared::ToolResult::fail("Grep failed: " + res.stderrData);
    }

    rapidjson::Document doc;
    doc.SetArray();
    auto &a = doc.GetAllocator();

    if (res.exitCode == 0) {
      std::istringstream stream(res.stdoutData);
      std::string line;
      // Regex to find :[0-9]+: (Match) or -[0-9]+- (Context)
      // We search for the FIRST occurrence which separates the filename from
      // the line number. Then the second separator follows the line number.
      static std::regex matchRegex(R"(:([0-9]+):)");
      static std::regex contextRegex(R"(-([0-9]+)-)");

      while (std::getline(stream, line)) {
        if (line.empty() || line == "--")
          continue;

        std::smatch m;
        bool isMatch = false;
        size_t sepPos = std::string::npos;
        size_t sepLen = 0;
        std::string lineStr;

        if (std::regex_search(line, m, matchRegex)) {
          isMatch = true;
          sepPos = m.position();
          sepLen = m.length();
          lineStr = m[1].str();
        } else if (std::regex_search(line, m, contextRegex)) {
          isMatch = false;
          sepPos = m.position();
          sepLen = m.length();
          lineStr = m[1].str();
        }

        if (sepPos == std::string::npos)
          continue;

        std::string file = line.substr(0, sepPos);
        std::string content = line.substr(sepPos + sepLen);

        int lineNum = 0;
        try {
          lineNum = std::stoi(lineStr);
        } catch (...) {
          continue;
        }

        rapidjson::Value val(rapidjson::kObjectType);
        val.AddMember("file", rapidjson::Value(file.c_str(), a).Move(), a);
        val.AddMember("line", lineNum, a);
        val.AddMember("content", rapidjson::Value(content.c_str(), a).Move(),
                      a);
        val.AddMember("is_match", isMatch, a);
        doc.PushBack(val, a);
      }
    }

    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
