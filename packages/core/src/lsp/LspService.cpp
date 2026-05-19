#include "lsp/LspService.hpp"

#include "IAgent.hpp"
#include "ITool.hpp"
#include "lsp/LspClient.hpp"
#include "lsp/LspProtocol.hpp"
#include "lsp/LspServerManager.hpp"
#include "lsp/LspServerRegistry.hpp"
#include "utils/ShellUtil.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace firmius::core {
namespace {

namespace fs = std::filesystem;

constexpr int kProjectBatchSettleMs = 500;
constexpr size_t kManagerStderrTailLines = 20;

std::string canonicalizePath(const std::string &path) {
  if (path.empty()) {
    return path;
  }

  std::error_code ec;
  const fs::path p(path);
  fs::path out = fs::weakly_canonical(p, ec);
  if (!ec) {
    return out.string();
  }

  ec.clear();
  out = fs::absolute(p, ec);
  if (!ec) {
    return out.lexically_normal().string();
  }

  return p.lexically_normal().string();
}

std::vector<std::string> runGitLsFiles(const std::string &root) {
  std::vector<std::string> lines;
  if (root.empty()) {
    return lines;
  }

  const std::string command =
      "git -C " + firmius::shared::shellQuoteSingle(root) + " ls-files 2>/dev/null";
  FILE *pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return lines;
  }

  std::array<char, 4096> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    std::string line = firmius::shared::StringUtil::trim(std::string_view(buffer.data()));
    if (!line.empty()) {
      lines.push_back(line);
    }
  }

  if (::pclose(pipe) != 0) {
    lines.clear();
  }
  return lines;
}

const LspServerSpec *chooseSpecForRoot(const std::string &root) {
  const auto &registry = LspServerRegistry::instance();

  for (const auto &rel : runGitLsFiles(root)) {
    const std::string file = canonicalizePath((fs::path(root) / rel).string());
    if (const auto *spec = registry.findByPath(file); spec != nullptr) {
      return spec;
    }
  }

  std::error_code ec;
  if (!fs::exists(root, ec) || ec) {
    return nullptr;
  }

  static const std::unordered_set<std::string> kSkipDirs = {
      ".git",        ".hg",         ".svn",      ".venv",
      "venv",        "node_modules", "dist",      "build",
      "target",      "__pycache__", ".mypy_cache", ".pytest_cache",
      ".ruff_cache", ".tox"};

  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  for (; it != end && !ec; it.increment(ec)) {
    const fs::path current = it->path();
    if (it->is_directory(ec)) {
      if (kSkipDirs.count(current.filename().string()) > 0) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const std::string file = canonicalizePath(current.string());
    if (const auto *spec = registry.findByPath(file); spec != nullptr) {
      return spec;
    }
  }

  return nullptr;
}

std::vector<std::string> candidateFiles(const std::string &root,
                                        const LspServerSpec &spec,
                                        int maxFiles) {
  std::vector<std::string> files;
  if (maxFiles <= 0 || root.empty()) {
    return files;
  }

  const std::unordered_set<std::string> extset(spec.extensions.begin(),
                                               spec.extensions.end());

  auto pushIfMatch = [&](const fs::path &path) {
    if (static_cast<int>(files.size()) >= maxFiles) {
      return;
    }
    if (extset.count(path.extension().string()) == 0) {
      return;
    }
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
      return;
    }
    files.push_back(canonicalizePath(path.string()));
  };

  for (const auto &rel : runGitLsFiles(root)) {
    pushIfMatch(fs::path(root) / rel);
    if (static_cast<int>(files.size()) >= maxFiles) {
      return files;
    }
  }

  static const std::unordered_set<std::string> kSkipDirs = {
      ".git",        ".hg",         ".svn",      ".venv",
      "venv",        "node_modules", "dist",      "build",
      "target",      "__pycache__", ".mypy_cache", ".pytest_cache",
      ".ruff_cache", ".tox"};

  std::error_code ec;
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  for (; it != end && !ec && static_cast<int>(files.size()) < maxFiles;
       it.increment(ec)) {
    const fs::path current = it->path();
    if (it->is_directory(ec)) {
      if (kSkipDirs.count(current.filename().string()) > 0) {
        it.disable_recursion_pending();
      }
      continue;
    }
    pushIfMatch(current);
  }

  return files;
}

rapidjson::Value diagnosticToPrettyValue(
    const Diagnostic &diagnostic,
    rapidjson::Document::AllocatorType &alloc) {
  const int severity = diagnostic.severity.has_value()
                           ? static_cast<int>(diagnostic.severity.value())
                           : 1;

  std::string severityName = "ERROR";
  if (severity == 2) {
    severityName = "WARN";
  } else if (severity == 3) {
    severityName = "INFO";
  } else if (severity >= 4) {
    severityName = "HINT";
  }

  const int line = diagnostic.range.start.line + 1;
  const int character = diagnostic.range.start.character + 1;
  const int endLine = diagnostic.range.end.line + 1;
  const int endCharacter = diagnostic.range.end.character + 1;

  std::ostringstream pretty;
  pretty << severityName << " [" << line << ":" << character << "] "
         << diagnostic.message;

  rapidjson::Value item(rapidjson::kObjectType);
  rapidjson::Value range(rapidjson::kObjectType);
  rapidjson::Value start(rapidjson::kObjectType);
  start.AddMember("line", diagnostic.range.start.line, alloc);
  start.AddMember("character", diagnostic.range.start.character, alloc);
  rapidjson::Value end(rapidjson::kObjectType);
  end.AddMember("line", diagnostic.range.end.line, alloc);
  end.AddMember("character", diagnostic.range.end.character, alloc);
  range.AddMember("start", start, alloc);
  range.AddMember("end", end, alloc);

  item.AddMember("range", range, alloc);
  item.AddMember("severity", severity, alloc);
  if (diagnostic.code.has_value()) {
    if (std::holds_alternative<int>(diagnostic.code.value())) {
      item.AddMember("code", std::get<int>(diagnostic.code.value()), alloc);
    } else {
      const std::string &code = std::get<std::string>(diagnostic.code.value());
      item.AddMember("code", rapidjson::Value(code.c_str(), alloc).Move(), alloc);
    }
  }
  if (diagnostic.source.has_value()) {
    item.AddMember("source",
                   rapidjson::Value(diagnostic.source->c_str(), alloc).Move(),
                   alloc);
  }
  item.AddMember("message", rapidjson::Value(diagnostic.message.c_str(), alloc).Move(),
                 alloc);
  item.AddMember("severity_name", rapidjson::Value(severityName.c_str(), alloc).Move(),
                 alloc);
  item.AddMember("line", line, alloc);
  item.AddMember("character", character, alloc);
  item.AddMember("end_line", endLine, alloc);
  item.AddMember("end_character", endCharacter, alloc);
  item.AddMember("pretty", rapidjson::Value(pretty.str().c_str(), alloc).Move(),
                 alloc);

  return item;
}

rapidjson::Value copyResultPayload(const rapidjson::Document &rpcDoc,
                                   rapidjson::Document::AllocatorType &alloc) {
  if (rpcDoc.IsObject() && rpcDoc.HasMember("result")) {
    rapidjson::Value result;
    result.CopyFrom(rpcDoc["result"], alloc);
    return result;
  }

  rapidjson::Value value;
  value.CopyFrom(rpcDoc, alloc);
  return value;
}

rapidjson::Value performSemanticRequest(LspClient *client,
                                        const LspRequest &request,
                                        const std::string &absolutePath,
                                        rapidjson::Document::AllocatorType &alloc) {
  const std::string uri = fileUri(absolutePath);
  const Position pos{std::max(request.line - 1, 0),
                     std::max(request.character - 1, 0)};

  if (request.operation == "hover") {
    return copyResultPayload(client->hover(uri, pos, request.timeout_ms), alloc);
  }
  if (request.operation == "definition") {
    return copyResultPayload(client->definition(uri, pos, request.timeout_ms), alloc);
  }
  if (request.operation == "references") {
    return copyResultPayload(
        client->references(uri, pos, request.include_declaration, request.timeout_ms),
        alloc);
  }
  if (request.operation == "implementation") {
    return copyResultPayload(client->implementation(uri, pos, request.timeout_ms),
                             alloc);
  }
  if (request.operation == "document_symbol") {
    return copyResultPayload(client->documentSymbol(uri, request.timeout_ms), alloc);
  }
  if (request.operation == "workspace_symbol") {
    return copyResultPayload(client->workspaceSymbol(request.query, request.timeout_ms),
                             alloc);
  }
  if (request.operation == "prepare_call_hierarchy") {
    return copyResultPayload(client->prepareCallHierarchy(uri, pos, request.timeout_ms),
                             alloc);
  }
  if (request.operation == "incoming_calls" ||
      request.operation == "outgoing_calls") {
    rapidjson::Document prepared =
        client->prepareCallHierarchy(uri, pos, request.timeout_ms);
    if (!prepared.IsObject() || !prepared.HasMember("result") ||
        !prepared["result"].IsArray() || prepared["result"].Empty()) {
      return rapidjson::Value(rapidjson::kArrayType);
    }

    rapidjson::Document calls;
    if (request.operation == "incoming_calls") {
      calls = client->incomingCalls(prepared["result"][0], request.timeout_ms);
    } else {
      calls = client->outgoingCalls(prepared["result"][0], request.timeout_ms);
    }
    return copyResultPayload(calls, alloc);
  }

  throw std::runtime_error("Unsupported operation: " + request.operation);
}

rapidjson::Document makeUnavailableResponse(const LspRequest &request,
                                            const std::string &path,
                                            const std::string &reason) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  doc.AddMember("ok", true, alloc);
  doc.AddMember("available", false, alloc);
  doc.AddMember("reason", rapidjson::Value(reason.c_str(), alloc).Move(), alloc);
  doc.AddMember("operation", rapidjson::Value(request.operation.c_str(), alloc).Move(),
                alloc);
  if (!path.empty()) {
    doc.AddMember("path", rapidjson::Value(path.c_str(), alloc).Move(), alloc);
  }
  doc.AddMember("project", request.project, alloc);
  if (!request.project_root.empty()) {
    doc.AddMember("project_root",
                  rapidjson::Value(request.project_root.c_str(), alloc).Move(), alloc);
  }
  return doc;
}

rapidjson::Document makeErrorResponse(const LspRequest &request,
                                      const std::string &path,
                                      const std::string &projectRoot,
                                      const std::string &error,
                                      const std::vector<std::string> &stderrLines) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  doc.AddMember("ok", false, alloc);
  doc.AddMember("available", false, alloc);
  doc.AddMember("operation", rapidjson::Value(request.operation.c_str(), alloc).Move(),
                alloc);
  if (!path.empty()) {
    doc.AddMember("path", rapidjson::Value(path.c_str(), alloc).Move(), alloc);
  }
  doc.AddMember("project", request.project, alloc);
  if (!projectRoot.empty()) {
    doc.AddMember("project_root", rapidjson::Value(projectRoot.c_str(), alloc).Move(),
                  alloc);
  }
  doc.AddMember("error", rapidjson::Value(error.c_str(), alloc).Move(), alloc);

  rapidjson::Value stderrArray(rapidjson::kArrayType);
  for (const auto &line : stderrLines) {
    stderrArray.PushBack(rapidjson::Value(line.c_str(), alloc).Move(), alloc);
  }
  doc.AddMember("stderr", stderrArray, alloc);
  return doc;
}

const rapidjson::Value *findDiagnosticArray(const rapidjson::Value *doc,
                                            const std::string &absolutePath) {
  if (doc == nullptr || !doc->IsObject() || !doc->HasMember("diagnostics") ||
      !(*doc)["diagnostics"].IsObject()) {
    return nullptr;
  }
  const auto &diagnostics = (*doc)["diagnostics"];
  if (!diagnostics.HasMember(absolutePath.c_str())) {
    return nullptr;
  }
  const auto &entry = diagnostics[absolutePath.c_str()];
  if (!entry.IsArray()) {
    return nullptr;
  }
  return &entry;
}

rapidjson::Value buildStringArray(const std::vector<std::string> &lines,
                                  rapidjson::Document::AllocatorType &alloc,
                                  int maxIssues) {
  rapidjson::Value array(rapidjson::kArrayType);
  const int limit = maxIssues > 0 ? maxIssues : static_cast<int>(lines.size());
  for (size_t i = 0; i < lines.size() && static_cast<int>(i) < limit; ++i) {
    array.PushBack(rapidjson::Value(lines[i].c_str(), alloc).Move(), alloc);
  }
  return array;
}

} // namespace

std::vector<std::string> collectDiagnosticPrettyLines(
    const rapidjson::Value *diagnosticsDoc,
    const std::string &absolutePath,
    int severity) {
  std::vector<std::string> lines;
  const auto *diagnostics = findDiagnosticArray(diagnosticsDoc, absolutePath);
  if (diagnostics == nullptr) {
    return lines;
  }
  for (const auto &issue : diagnostics->GetArray()) {
    if (!issue.IsObject()) {
      continue;
    }
    const int issueSeverity =
        issue.HasMember("severity") && issue["severity"].IsInt()
            ? issue["severity"].GetInt()
            : 1;
    if (issueSeverity != severity) {
      continue;
    }
    if (!issue.HasMember("pretty") || !issue["pretty"].IsString()) {
      continue;
    }
    lines.emplace_back(issue["pretty"].GetString());
  }
  return lines;
}

rapidjson::Document runLspRequest(const LspRequest &request,
                                  const std::string &cwd,
                                  std::string *error) {
  const std::string absolutePath = canonicalizePath(request.path);

  const LspServerSpec *spec = nullptr;
  if (!absolutePath.empty()) {
    spec = LspServerRegistry::instance().findByPath(absolutePath);
  }

  std::string projectRootHint =
      request.project_root.empty() ? canonicalizePath(cwd)
                                   : canonicalizePath(request.project_root);
  if (spec == nullptr && absolutePath.empty() && !projectRootHint.empty()) {
    spec = chooseSpecForRoot(projectRootHint);
  }

  if (spec == nullptr) {
    return makeUnavailableResponse(request, absolutePath,
                                   "No LSP server mapping for this path.");
  }

  std::string projectRoot;
  if (!request.project_root.empty()) {
    projectRoot = canonicalizePath(request.project_root);
  } else {
    const std::string rootStart = !absolutePath.empty() ? absolutePath : cwd;
    projectRoot = canonicalizePath(
        LspServerRegistry::detectRoot(rootStart, spec->markers));
  }

  std::vector<std::string> serverCommand;

  try {
    LspClient *client = LspServerManager::instance().getOrCreateServer(
        *spec, projectRoot, request.timeout_ms);

    if (!absolutePath.empty() && fs::exists(absolutePath)) {
      const std::string languageId = spec->languageIdForPath(absolutePath);
      serverCommand = spec->resolveCommand();
      const bool waitForFileDiagnostics = !request.project;
      client->touchFile(absolutePath, languageId, waitForFileDiagnostics,
                        request.timeout_ms);
    }

    if (request.project) {
      const auto files = candidateFiles(projectRoot, *spec, request.max_files);
      for (const auto &file : files) {
        client->touchFile(file, spec->languageIdForPath(file), false,
                          request.timeout_ms);
      }
      if (!files.empty()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kProjectBatchSettleMs));
      }
    }

    rapidjson::Document doc(rapidjson::kObjectType);
    auto &alloc = doc.GetAllocator();

    doc.AddMember("ok", true, alloc);
    doc.AddMember("available", true, alloc);
    doc.AddMember("operation", rapidjson::Value(request.operation.c_str(), alloc).Move(),
                  alloc);
    if (!absolutePath.empty()) {
      doc.AddMember("path", rapidjson::Value(absolutePath.c_str(), alloc).Move(),
                    alloc);
    } else {
      doc.AddMember("path", rapidjson::Value(rapidjson::kNullType), alloc);
    }
    doc.AddMember("project", request.project, alloc);
    doc.AddMember("project_root", rapidjson::Value(projectRoot.c_str(), alloc).Move(),
                  alloc);
    doc.AddMember("server_id", rapidjson::Value(spec->id.c_str(), alloc).Move(),
                  alloc);

    rapidjson::Value commandArray(rapidjson::kArrayType);
    for (const auto &arg : serverCommand) {
      commandArray.PushBack(rapidjson::Value(arg.c_str(), alloc).Move(), alloc);
    }
    doc.AddMember("server_command", commandArray, alloc);

    rapidjson::Value diagnosticsObj(rapidjson::kObjectType);
    DiagnosticSummary summary;
    const auto diagnosticsByUri = client->getAllDiagnostics();
    for (const auto &[uri, diagnostics] : diagnosticsByUri) {
      const std::string path = canonicalizePath(pathFromUri(uri));
      rapidjson::Value issues(rapidjson::kArrayType);
      for (const auto &diag : diagnostics) {
        const int severity = diag.severity.has_value()
                                 ? static_cast<int>(diag.severity.value())
                                 : 1;
        if (severity == 1) {
          summary.errors += 1;
        } else if (severity == 2) {
          summary.warnings += 1;
        } else if (severity == 3) {
          summary.infos += 1;
        } else {
          summary.hints += 1;
        }
        issues.PushBack(diagnosticToPrettyValue(diag, alloc), alloc);
      }
      if (!diagnostics.empty()) {
        summary.files += 1;
      }
      diagnosticsObj.AddMember(rapidjson::Value(path.c_str(), alloc).Move(), issues,
                               alloc);
    }
    doc.AddMember("diagnostics", diagnosticsObj, alloc);

    rapidjson::Value summaryObj(rapidjson::kObjectType);
    summaryObj.AddMember("files", summary.files, alloc);
    summaryObj.AddMember("errors", summary.errors, alloc);
    summaryObj.AddMember("warnings", summary.warnings, alloc);
    summaryObj.AddMember("infos", summary.infos, alloc);
    summaryObj.AddMember("hints", summary.hints, alloc);
    doc.AddMember("summary", summaryObj, alloc);

    if (request.operation != "diagnostics") {
      if (request.operation == "workspace_symbol") {
        doc.AddMember(
            "result",
            copyResultPayload(
                client->workspaceSymbol(request.query, request.timeout_ms), alloc),
            alloc);
      } else if (!absolutePath.empty() && fs::exists(absolutePath)) {
        doc.AddMember("result",
                      performSemanticRequest(client, request, absolutePath, alloc),
                      alloc);
      } else {
        throw std::runtime_error("Operation requires an existing path: " +
                                 request.operation);
      }
    } else {
      doc.AddMember("result", rapidjson::Value(rapidjson::kNullType), alloc);
    }

    const auto stderrLines = LspServerManager::instance().getServerStderr(
        spec->id, projectRoot, kManagerStderrTailLines);
    rapidjson::Value stderrArray(rapidjson::kArrayType);
    for (const auto &line : stderrLines) {
      stderrArray.PushBack(rapidjson::Value(line.c_str(), alloc).Move(), alloc);
    }
    doc.AddMember("stderr", stderrArray, alloc);

    return doc;
  } catch (const std::exception &ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    const auto stderrLines = LspServerManager::instance().getServerStderr(
        spec->id, projectRoot, kManagerStderrTailLines);
    return makeErrorResponse(request, absolutePath, projectRoot, ex.what(),
                             stderrLines);
  }
}

rapidjson::Document collectFileLspDiagnostics(const std::string &absolutePath,
                                              shared::ToolContext &ctx,
                                              bool project,
                                              int maxFiles,
                                              int timeoutMs) {
  LspRequest request;
  request.operation = "diagnostics";
  request.path = absolutePath;
  request.project_root =
      ctx.agent.getEnvironment()->getWorkspace().getCurrentWorkingDirectory();
  request.project = project;
  request.max_files = maxFiles;
  request.timeout_ms = timeoutMs;
  return runLspRequest(
      request,
      ctx.agent.getEnvironment()->getWorkspace().getCurrentWorkingDirectory());
}

void attachFileEditLspSummary(rapidjson::Document &target,
                              const std::string &absolutePath,
                              const rapidjson::Document *before,
                              const rapidjson::Document *after,
                              int maxIssues) {
  auto &alloc = target.GetAllocator();
  rapidjson::Value lsp(rapidjson::kObjectType);

  const bool checked = after != nullptr && after->IsObject() &&
                       after->HasMember("ok") && (*after)["ok"].IsBool();
  const bool available = checked && after->HasMember("available") &&
                         (*after)["available"].IsBool() &&
                         (*after)["available"].GetBool();
  lsp.AddMember("checked", checked, alloc);
  lsp.AddMember("available", available, alloc);

  if (after != nullptr && after->IsObject()) {
    if (after->HasMember("server_id") && (*after)["server_id"].IsString()) {
      lsp.AddMember(
          "server_id",
          rapidjson::Value((*after)["server_id"].GetString(), alloc).Move(), alloc);
    }
    if (after->HasMember("error") && (*after)["error"].IsString()) {
      lsp.AddMember("error",
                    rapidjson::Value((*after)["error"].GetString(), alloc).Move(),
                    alloc);
    }
    if (after->HasMember("summary") && (*after)["summary"].IsObject()) {
      const auto &summary = (*after)["summary"];
      if (summary.HasMember("errors") && summary["errors"].IsInt()) {
        lsp.AddMember("errors", summary["errors"].GetInt(), alloc);
      }
      if (summary.HasMember("warnings") && summary["warnings"].IsInt()) {
        lsp.AddMember("warnings", summary["warnings"].GetInt(), alloc);
      }
      if (summary.HasMember("infos") && summary["infos"].IsInt()) {
        lsp.AddMember("infos", summary["infos"].GetInt(), alloc);
      }
      if (summary.HasMember("hints") && summary["hints"].IsInt()) {
        lsp.AddMember("hints", summary["hints"].GetInt(), alloc);
      }
    }
  }

  const auto beforeErrors = collectDiagnosticPrettyLines(before, absolutePath, 1);
  const auto beforeWarnings = collectDiagnosticPrettyLines(before, absolutePath, 2);
  const auto afterErrors = collectDiagnosticPrettyLines(after, absolutePath, 1);
  const auto afterWarnings = collectDiagnosticPrettyLines(after, absolutePath, 2);

  std::set<std::string> seenErrors(beforeErrors.begin(), beforeErrors.end());
  std::vector<std::string> newErrors;
  for (const auto &line : afterErrors) {
    if (!seenErrors.count(line)) {
      newErrors.push_back(line);
    }
  }

  std::set<std::string> seenWarnings(beforeWarnings.begin(), beforeWarnings.end());
  std::vector<std::string> newWarnings;
  for (const auto &line : afterWarnings) {
    if (!seenWarnings.count(line)) {
      newWarnings.push_back(line);
    }
  }

  lsp.AddMember("issues", buildStringArray(afterErrors, alloc, maxIssues), alloc);
  rapidjson::Value warningIssues = buildStringArray(afterWarnings, alloc, maxIssues);
  lsp.AddMember("warning_issues", warningIssues, alloc);
  lsp.AddMember("new_errors", buildStringArray(newErrors, alloc, maxIssues), alloc);
  lsp.AddMember("new_warnings", buildStringArray(newWarnings, alloc, maxIssues),
                alloc);
  lsp.AddMember("new_error_count", static_cast<int>(newErrors.size()), alloc);
  lsp.AddMember("new_warning_count", static_cast<int>(newWarnings.size()), alloc);

  target.AddMember("lsp", lsp, alloc);
}

} // namespace firmius::core
