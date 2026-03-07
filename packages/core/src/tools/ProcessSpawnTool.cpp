#include "tools/ProcessSpawnTool.hpp"
#include "IAgent.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

shared::ToolResult ProcessSpawnTool::execute(const ProcessSpawnInput &input,
                                             shared::ToolContext &ctx) {
  try {
    std::string processId = ctx.agent.spawnProcess(
        input.command, ctx.currentToolCallId, input.cwd, input.env);

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
