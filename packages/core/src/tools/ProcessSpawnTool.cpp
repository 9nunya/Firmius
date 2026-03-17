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
    effectiveCwd = ctx.agent.getEnvironment()->getWorkspace().resolvePath(effectiveCwd);

    ctx.agent.getPermissions()->validatePathAccess(effectiveCwd, firmius::shared::AccessMode::READ);
    auto intent = ctx.agent.getPermissions()->getIntentAnalyzer().analyze(
        input.command, effectiveCwd);
    auto approval =
        ctx.agent.getPermissions()->requestCommandApproval(input.command, intent);
    if (approval == PermissionResponse::Deny) {
      return shared::ToolResult::fail("Command execution denied: " +
                                      input.command);
    }

    std::string processId = ctx.agent.getEnvironment()->getProcessManager().spawnProcess(
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
