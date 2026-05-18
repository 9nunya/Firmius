#include "tools/LspDiagnosticsTool.hpp"
#include "agents/Agent.hpp"
#include "lsp/LspService.hpp"
#include <rapidjson/document.h>
#include <sstream>
#include <string>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata LspDiagnosticsTool::getMetadata() const {
  return {"lsp_diagnostics",
          "Language-server operations: run diagnostics for a file or project slice and return warnings/errors.",
          shared::ToolScope::Semantic};
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
    auto bridgeDoc = runLspRequest(request, request.project_root, &error);

    // Token-waste pass 3: prose-first diagnostics. The bridge returns a
    // verbose `diagnostics: {path: [{severity,severity_name,source,code,
    // message,pretty,range:{...}}, ...]}` shape with up to 7 fields per
    // diagnostic. The model only needs the one-line `pretty` form to act
    // on each issue; everything else is rederivable. We pass through the
    // top-level `summary` counters but flatten the diagnostics into one
    // string per issue ("ERROR src/foo.cpp:12:5 message").
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();

    int errors = 0, warnings = 0, infos = 0, hints = 0, files = 0;
    if (bridgeDoc.HasMember("summary") && bridgeDoc["summary"].IsObject()) {
      const auto &s = bridgeDoc["summary"];
      if (s.HasMember("errors") && s["errors"].IsInt()) errors = s["errors"].GetInt();
      if (s.HasMember("warnings") && s["warnings"].IsInt()) warnings = s["warnings"].GetInt();
      if (s.HasMember("infos") && s["infos"].IsInt()) infos = s["infos"].GetInt();
      if (s.HasMember("hints") && s["hints"].IsInt()) hints = s["hints"].GetInt();
      if (s.HasMember("files") && s["files"].IsInt()) files = s["files"].GetInt();
    }

    std::ostringstream prose;
    if (bridgeDoc.HasMember("ok") && bridgeDoc["ok"].IsBool() &&
        !bridgeDoc["ok"].GetBool()) {
      const std::string err =
          (bridgeDoc.HasMember("error") && bridgeDoc["error"].IsString())
              ? bridgeDoc["error"].GetString()
              : "LSP bridge returned an error";
      prose << "LSP diagnostics unavailable: " << err << ".";
      doc.AddMember(
          "result",
          rapidjson::Value(prose.str().c_str(),
                           static_cast<rapidjson::SizeType>(prose.str().size()),
                           alloc).Move(),
          alloc);
      doc.AddMember("ok", false, alloc);
      return shared::ToolResult::ok(doc);
    }

    if (errors == 0 && warnings == 0 && infos == 0 && hints == 0) {
      prose << "No diagnostics across " << files << " file"
            << (files == 1 ? "" : "s") << ".";
    } else {
      prose << errors << " error" << (errors == 1 ? "" : "s") << ", "
            << warnings << " warning" << (warnings == 1 ? "" : "s");
      if (infos > 0) prose << ", " << infos << " info"
                           << (infos == 1 ? "" : "s");
      if (hints > 0) prose << ", " << hints << " hint"
                           << (hints == 1 ? "" : "s");
      prose << " across " << files << " file"
            << (files == 1 ? "" : "s") << ":\n";

      // Inline each diagnostic as one prose line. We use the bridge's
      // pre-built `pretty` field when present (form: "SEVERITY [L:C]
      // message") and prefix with the file path for cross-file scans.
      if (bridgeDoc.HasMember("diagnostics") &&
          bridgeDoc["diagnostics"].IsObject()) {
        for (auto it = bridgeDoc["diagnostics"].MemberBegin();
             it != bridgeDoc["diagnostics"].MemberEnd(); ++it) {
          if (!it->value.IsArray()) continue;
          const std::string filePath = it->name.GetString();
          for (const auto &diag : it->value.GetArray()) {
            if (!diag.IsObject()) continue;
            std::string pretty;
            if (diag.HasMember("pretty") && diag["pretty"].IsString()) {
              pretty = diag["pretty"].GetString();
            } else {
              // Fallback: synthesize a pretty line from severity+message.
              std::string severityName = "ERROR";
              if (diag.HasMember("severity_name") &&
                  diag["severity_name"].IsString()) {
                severityName = diag["severity_name"].GetString();
              }
              std::string message;
              if (diag.HasMember("message") && diag["message"].IsString()) {
                message = diag["message"].GetString();
              }
              pretty = severityName + " " + message;
            }
            prose << "  " << filePath << ": " << pretty << "\n";
          }
        }
      }
    }

    const std::string proseStr = prose.str();
    doc.AddMember(
        "result",
        rapidjson::Value(proseStr.c_str(),
                         static_cast<rapidjson::SizeType>(proseStr.size()),
                         alloc).Move(),
        alloc);
    doc.AddMember("errors", errors, alloc);
    doc.AddMember("warnings", warnings, alloc);
    if (infos > 0) doc.AddMember("infos", infos, alloc);
    if (hints > 0) doc.AddMember("hints", hints, alloc);
    doc.AddMember("files", files, alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
