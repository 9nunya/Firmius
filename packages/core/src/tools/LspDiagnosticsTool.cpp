#include "tools/LspDiagnosticsTool.hpp"
#include "agents/Agent.hpp"
#include "lsp/LspService.hpp"
#include <rapidjson/document.h>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata LspDiagnosticsTool::getMetadata() const {
  return {"lsp_diagnostics",
          "Run real LSP diagnostics for one file or a project slice and return warnings/errors.",
          ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> LspDiagnosticsTool::getSchema() const {
  return zObject({
             {"path", zString()->describe("Absolute or relative path to a source file")->setOptional()},
             {"project_root", zString()->describe("Optional project root override")->setOptional()},
             {"project", zBoolean()->describe("When true, scan a project slice for diagnostics using the file type of path")->setOptional()},
             {"max_files", zInteger()->describe("Maximum files to scan in project mode (default 50)")->setOptional()},
             {"timeout_ms", zInteger()->describe("Timeout for the bridge request in milliseconds (default 30000)")->setOptional()},
         })
      ->required({});
}

LspDiagnosticsInput LspDiagnosticsTool::transform(const rapidjson::Value &json) {
  LspDiagnosticsInput input;
  MAP_STRING(path, "path");
  MAP_STRING(project_root, "project_root");
  MAP_BOOL(project, "project");
  MAP_INT(max_files, "max_files");
  MAP_INT(timeout_ms, "timeout_ms");
  return input;
}

shared::ToolResult LspDiagnosticsTool::execute(const LspDiagnosticsInput &input,
                                               shared::ToolContext &ctx) {
  try {
    LspRequest request;
    request.operation = "diagnostics";
    request.project = input.project;
    request.max_files = input.max_files > 0 ? input.max_files : 50;
    request.timeout_ms = input.timeout_ms > 0 ? input.timeout_ms : 30000;

    if (!input.path.empty()) {
      request.path = ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.path);
      ctx.agent.getPermissions()->validatePathAccess(request.path, AccessMode::READ);
    }
    if (!input.project_root.empty()) {
      request.project_root =
          ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.project_root);
      ctx.agent.getPermissions()->validatePathAccess(request.project_root,
                                                     AccessMode::READ);
    } else {
      request.project_root =
          ctx.agent.getEnvironment()->getWorkspace().getCurrentWorkingDirectory();
    }

    std::string error;
    auto doc = runLspRequest(request, request.project_root, &error);
    if (doc.HasMember("path") && !doc["path"].IsString() && !request.path.empty()) {
      doc.RemoveMember("path");
      doc.AddMember("path",
                    rapidjson::Value(request.path.c_str(), doc.GetAllocator()).Move(),
                    doc.GetAllocator());
    }
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
