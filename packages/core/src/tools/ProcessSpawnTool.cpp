#include "tools/ProcessSpawnTool.hpp"
#include "IAgent.hpp"
#include "utils/FSUtil.hpp"
#include <rapidjson/document.h>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolResult ProcessSpawnTool::execute(const ProcessSpawnInput &input,
                                             shared::ToolContext &ctx) {
  try {
    std::string effectiveCwd =
        input.cwd.empty() ? ctx.agent.getContext().environment.cwd : input.cwd;
    effectiveCwd = ctx.agent.resolvePath(effectiveCwd);

    // Security check
    bool allowed = false;
    for (const auto& p : ctx.agent.getContext().permissions.allowedPaths) {
        if (FSUtil::isSubpath(effectiveCwd, p)) {
            allowed = true;
            break;
        }
    }
    if (!allowed && !ctx.agent.getContext().permissions.allowOutsideCwd) {
        return shared::ToolResult::fail("Access denied: cwd outside allowed directories: " + effectiveCwd);
    }

    std::string processId = ctx.agent.spawnProcess(
        input.command, ctx.currentToolCallId, effectiveCwd, input.env);

    rapidjson::Document doc;
    doc.SetObject();
    doc.AddMember(
        "process_id",
        rapidjson::Value(processId.c_str(), doc.GetAllocator()).Move(),
        doc.GetAllocator());

    return shared::ToolResult::ok(doc, processId);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
