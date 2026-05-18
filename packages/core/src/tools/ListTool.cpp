#include "tools/ListTool.hpp"

#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include <cstdint>
#include <sstream>
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

    // Token-waste pass 3: prose-first directory listing.
    //
    // Old shape was an array of {name,path,size,is_directory,is_symlink,
    // modified_ms} per entry — six fields per file, multiplied by however
    // many entries the directory has. The model rarely needs mtimes; the
    // common questions are "what's in this dir?" and "is this a file or
    // a directory?". Both answer themselves from one prose line per entry
    // with a trailing slash on directories and a → indicator on symlinks.
    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();

    auto fmtBytes = [](std::uint64_t n) {
      std::ostringstream o;
      if (n < 1024) {
        o << n << "B";
      } else if (n < 1024ull * 1024) {
        o.precision(1);
        o << std::fixed << (static_cast<double>(n) / 1024.0) << "K";
      } else {
        o.precision(1);
        o << std::fixed << (static_cast<double>(n) / (1024.0 * 1024.0)) << "M";
      }
      return o.str();
    };

    std::ostringstream prose;
    int dirCount = 0;
    int fileCount = 0;
    int linkCount = 0;
    int shown = 0;
    for (const auto &entry : entries) {
      if (!include_hidden && !entry.name.empty() && entry.name[0] == '.') {
        continue;
      }
      if (entry.isDirectory) ++dirCount;
      else ++fileCount;
      if (entry.isSymlink) ++linkCount;
      ++shown;
    }

    if (shown == 0) {
      prose << "Directory " << path << " is empty.";
    } else {
      prose << shown << " entries in " << path << " (" << dirCount
            << " director" << (dirCount == 1 ? "y" : "ies") << ", "
            << fileCount << " file" << (fileCount == 1 ? "" : "s");
      if (linkCount > 0) prose << ", " << linkCount << " symlink"
                               << (linkCount == 1 ? "" : "s");
      prose << "):\n";
      for (const auto &entry : entries) {
        if (!include_hidden && !entry.name.empty() && entry.name[0] == '.') {
          continue;
        }
        prose << "  " << entry.name;
        if (entry.isDirectory) prose << "/";
        if (entry.isSymlink) prose << " →";
        if (!entry.isDirectory) prose << " (" << fmtBytes(entry.size) << ")";
        prose << "\n";
      }
    }

    std::string proseStr = prose.str();
    doc.AddMember(
        "result",
        rapidjson::Value(proseStr.c_str(),
                         static_cast<rapidjson::SizeType>(proseStr.size()),
                         a).Move(),
        a);
    doc.AddMember("count", shown, a);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
