#ifndef FIRMIUS_CORE_LSP_SERVICE_HPP
#define FIRMIUS_CORE_LSP_SERVICE_HPP

#include <rapidjson/fwd.h>

#include <string>
#include <vector>

namespace firmius::shared {
struct ToolContext;
}

namespace firmius::core {

struct LspRequest {
  std::string operation = "diagnostics";
  std::string path;
  std::string project_root;
  std::string query;
  int line = 1;
  int character = 1;
  bool project = false;
  bool include_declaration = true;
  int max_files = 50;
  int max_results = 50;
  int timeout_ms = 30000;
};

rapidjson::Document runLspRequest(const LspRequest &request,
                                  const std::string &cwd,
                                  std::string *error = nullptr);

rapidjson::Document collectFileLspDiagnostics(const std::string &absolutePath,
                                              shared::ToolContext &ctx,
                                              bool project = false,
                                              int maxFiles = 50,
                                              int timeoutMs = 30000);

void attachFileEditLspSummary(rapidjson::Document &target,
                              const std::string &absolutePath,
                              const rapidjson::Document *before,
                              const rapidjson::Document *after,
                              int maxIssues = 8);

std::vector<std::string> collectDiagnosticPrettyLines(
    const rapidjson::Value *diagnosticsDoc,
    const std::string &absolutePath,
    int severity);

} // namespace firmius::core

#endif // FIRMIUS_CORE_LSP_SERVICE_HPP
