#include "tools/ListTool.hpp"

#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata ListTool::getMetadata() const {
  return {
      "List",
      R"(List directory entries.

USAGE GUIDANCE:
- Use List to discover files and directories.
- Always pass paths relative to the workspace root when possible.

SECURITY / PERMISSIONS:
- Requires filesystem READ access to the resolved path.
)",
      shared::ToolScope::FilesystemRead};
}

std::shared_ptr<shared::JSONSchema> ListTool::getSchema() const {
  return shared::zObject({
      {"path",
       shared::zString()
           ->describe(
               "The directory to list. Provide a workspace-relative path when possible. "
               "Security: must be readable under current permissions. Relative paths are resolved against the workspace root.")},
      {"include_hidden",
       shared::zBoolean()
           ->setOptional()
           ->describe(
               "Whether to include dotfiles/directories (names starting with '.'). Default: false.")},
  });
}

shared::ToolResult ListTool::execute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string path;
  if (input.HasMember("path") && input["path"].IsString()) {
    path = input["path"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: path");
  }

  bool include_hidden = false;
  if (input.HasMember("include_hidden") && input["include_hidden"].IsBool()) {
    include_hidden = input["include_hidden"].GetBool();
  }

  try {
    std::string absPath = ctx.agent.getEnvironment()->getWorkspace().resolvePath(path);
    ctx.agent.getPermissions()->validatePathAccess(absPath, firmius::shared::AccessMode::READ);

    auto entries = ctx.host.listDir(absPath);

    rapidjson::Document doc;
    doc.SetArray();
    auto &a = doc.GetAllocator();

    for (const auto &entry : entries) {
      if (!include_hidden && !entry.name.empty() && entry.name[0] == '.') {
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
