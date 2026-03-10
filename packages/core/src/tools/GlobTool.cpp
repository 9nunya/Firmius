#include "tools/GlobTool.hpp"
#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include <filesystem>
#include <rapidjson/document.h>
#include <sstream>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolResult GlobTool::execute(const GlobInput &input,
                                     shared::ToolContext &ctx) {
  try {
    std::string absPath = ctx.agent.resolvePath(input.path);

    // Security check
    ctx.agent.getPermissionChecks().validatePathAccess(absPath);

    std::string command = "find " + shared::StringUtil::shellEscape(absPath) +
                          " -name " +
                          shared::StringUtil::shellEscape(input.pattern);

    auto res = ctx.host.exec(command);
    if (res.exitCode != 0 && res.exitCode != 1) {
      return shared::ToolResult::fail("Glob failed: " + res.stderrData);
    }

    rapidjson::Document doc;
    doc.SetArray();
    auto &a = doc.GetAllocator();

    std::istringstream stream(res.stdoutData);
    std::string line;
    while (std::getline(stream, line)) {
      if (!line.empty()) {
        doc.PushBack(rapidjson::Value(line.c_str(), a).Move(), a);
      }
    }

    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}
} // namespace firmius::core
