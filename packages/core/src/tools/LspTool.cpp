#include "tools/LspTool.hpp"

#include "tools/LspDiagnosticsTool.hpp"
#include "tools/LspQueryTool.hpp"

#include <rapidjson/document.h>

namespace firmius::core {

namespace {

std::string getActionString(const rapidjson::Value &input) {
  if (!input.IsObject() || !input.HasMember("action") ||
      !input["action"].IsString()) {
    return "";
  }
  return input["action"].GetString();
}

rapidjson::Document forwardedArgsWithoutAction(const rapidjson::Value &input) {
  rapidjson::Document forwarded;
  forwarded.SetObject();
  auto &alloc = forwarded.GetAllocator();
  if (!input.IsObject()) {
    return forwarded;
  }

  for (auto it = input.MemberBegin(); it != input.MemberEnd(); ++it) {
    if (std::string_view(it->name.GetString()) == "action") {
      continue;
    }
    rapidjson::Value key(it->name.GetString(), alloc);
    rapidjson::Value value;
    value.CopyFrom(it->value, alloc);
    forwarded.AddMember(key.Move(), value.Move(), alloc);
  }

  return forwarded;
}

template <typename Tool>
shared::ToolResult forwardTool(Tool &tool, const rapidjson::Value &input,
                               shared::ToolContext &ctx) {
  const rapidjson::Document forwarded = forwardedArgsWithoutAction(input);
  auto validation = tool.getSchema()->validate(forwarded);
  if (!validation.success) {
    return shared::ToolResult::fail(validation.violationToPretty());
  }
  return static_cast<shared::ITool &>(tool).execute(forwarded, ctx);
}

} // namespace

shared::ToolMetadata LspTool::getMetadata() const {
  return {"Lsp",
          "Language-server operations. Use action Query or Diagnostics.",
          shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> LspTool::getSchema() const {
  return shared::zObject({
      {"action", shared::zEnum({"Query", "Diagnostics"})
                     ->describe(
                         "Which language-server operation family to execute.\n\n"
                         "- Query: semantic point query/navigation operation\n"
                         "- Diagnostics: retrieve diagnostics for a file/project surface")},
      {"operation", shared::zString()->setOptional()->describe(
          "For action=Query, the specific LSP semantic operation (for example hover, definition, references, implementation, document_symbol, workspace_symbol, prepare_call_hierarchy, incoming_calls, outgoing_calls).")},
      {"path", shared::zString()->setOptional()->describe(
          "Primary file path for the query/diagnostics operation. Prefer workspace-relative paths.")},
      {"project_root", shared::zString()->setOptional()->describe(
          "Optional project root override for LSP resolution when the workspace contains multiple projects.")},
  });
}

shared::ToolResult LspTool::execute(const rapidjson::Value &input,
                                           shared::ToolContext &ctx) {
  const std::string action = getActionString(input);
  if (action == "Query") {
    LspQueryTool tool;
    return forwardTool(tool, input, ctx);
  }
  if (action == "Diagnostics") {
    LspDiagnosticsTool tool;
    return forwardTool(tool, input, ctx);
  }
  return shared::ToolResult::fail("Lsp.action must be Query or Diagnostics");
}

} // namespace firmius::core
