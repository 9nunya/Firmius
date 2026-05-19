#ifndef FIRMIUS_CORE_LSPDIAGNOSTICSTOOL_HPP
#define FIRMIUS_CORE_LSPDIAGNOSTICSTOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

struct LspDiagnosticsInput {
  std::string path;
  std::string project_root;
  bool project = false;
  int max_files = 50;
  int timeout_ms = 30000;
};

class LspDiagnosticsTool : public shared::TypedTool<LspDiagnosticsInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;
  LspDiagnosticsInput transform(const rapidjson::Value &json) override;
  shared::ToolResult execute(const LspDiagnosticsInput &input,
                             shared::ToolContext &ctx) override;
};

}

#endif
