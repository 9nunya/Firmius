#include "tools/ProcessSpawnTool.hpp"
#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include <rapidjson/document.h>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolResult ProcessSpawnTool::execute(const ProcessSpawnInput &input,
                                             shared::ToolContext &ctx) {
  try {
    std::string effectiveCwd =
        input.cwd.empty() ? ctx.agent.getContext().environment.cwd : input.cwd;
    effectiveCwd = ctx.agent.resolvePath(effectiveCwd);

    ctx.agent.getPermissionChecks().validatePathAccess(effectiveCwd);

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
