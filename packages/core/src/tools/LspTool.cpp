#include "tools/LspTool.hpp"
#include "agents/Agent.hpp"
#include "lsp/LspService.hpp"
#include <rapidjson/document.h>
#include <set>
#include <stdexcept>

namespace firmius::core {
using namespace firmius::shared;

namespace {
const std::set<std::string> kPositionOperations = {
    "hover",           "definition",      "references",
    "implementation",  "prepare_call_hierarchy",
    "incoming_calls",  "outgoing_calls",
};
const std::set<std::string> kPathOnlyOperations = {"document_symbol"};
const std::set<std::string> kQueryOperations = {"workspace_symbol"};

void validateInput(const LspToolInput &input) {
  if (input.operation.empty()) {
    throw std::runtime_error("lsp requires an operation");
  }
  if (kPositionOperations.count(input.operation)) {
    if (input.path.empty()) {
      throw std::runtime_error(input.operation + " requires path");
    }
    if (input.line <= 0 || input.character <= 0) {
      throw std::runtime_error(input.operation + " requires positive line and character");
    }
    return;
  }
  if (kPathOnlyOperations.count(input.operation)) {
    if (input.path.empty()) {
      throw std::runtime_error(input.operation + " requires path");
    }
    return;
  }
  if (kQueryOperations.count(input.operation)) {
    return;
  }
  throw std::runtime_error("Unsupported lsp operation: " + input.operation);
}
} // namespace

shared::ToolMetadata LspTool::getMetadata() const {
  return {"lsp",
          "Run real LSP semantic queries such as hover, definition, references, document symbols, and workspace symbols.",
          ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> LspTool::getSchema() const {
  return zObject({
             {"operation", zEnum({"hover", "definition", "references", "implementation", "document_symbol", "workspace_symbol", "prepare_call_hierarchy", "incoming_calls", "outgoing_calls"})
                                ->describe("LSP operation to execute")},
             {"path", zString()->describe("Absolute or relative source file path")->setOptional()},
             {"project_root", zString()->describe("Optional project root override")->setOptional()},
             {"query", zString()->describe("Workspace symbol query")->setOptional()},
             {"line", zInteger()->describe("1-based line number for position-based operations")->setOptional()},
             {"character", zInteger()->describe("1-based character number for position-based operations")->setOptional()},
             {"include_declaration", zBoolean()->describe("Include declaration in references results")->setOptional()},
             {"max_results", zInteger()->describe("Reserved result cap for future filtering")->setOptional()},
             {"timeout_ms", zInteger()->describe("Timeout for the bridge request in milliseconds (default 30000)")->setOptional()},
         })
      ->required({"operation"});
}

LspToolInput LspTool::transform(const rapidjson::Value &json) {
  LspToolInput input;
  MAP_STRING(operation, "operation");
  MAP_STRING(path, "path");
  MAP_STRING(project_root, "project_root");
  MAP_STRING(query, "query");
  MAP_INT(line, "line");
  MAP_INT(character, "character");
  MAP_BOOL(include_declaration, "include_declaration");
  MAP_INT(max_results, "max_results");
  MAP_INT(timeout_ms, "timeout_ms");
  return input;
}

shared::ToolResult LspTool::execute(const LspToolInput &input,
                                    shared::ToolContext &ctx) {
  try {
    validateInput(input);

    LspRequest request;
    request.operation = input.operation;
    request.query = input.query;
    request.line = input.line > 0 ? input.line : 1;
    request.character = input.character > 0 ? input.character : 1;
    request.include_declaration = input.include_declaration;
    request.max_results = input.max_results > 0 ? input.max_results : 50;
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

    auto doc = runLspRequest(request, request.project_root);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core
