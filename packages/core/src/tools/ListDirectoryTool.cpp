#include "tools/ListDirectoryTool.hpp"
#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

shared::ToolResult ListDirectoryTool::execute(const ListDirectoryInput &input,
                                              shared::ToolContext &ctx) {
  try {
    std::string absPath = ctx.agent.resolvePath(input.path);

    ctx.agent.getPermissionChecks().validatePathAccess(absPath);

    auto entries = ctx.host.listDir(absPath);

    rapidjson::Document doc;
    doc.SetArray();
    auto &a = doc.GetAllocator();

    for (const auto &entry : entries) {
      if (!input.include_hidden && !entry.name.empty() &&
          entry.name[0] == '.') {
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

} // namespace firmius::core
