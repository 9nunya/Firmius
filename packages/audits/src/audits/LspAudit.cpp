#include "audits/LspAudit.hpp"
#include "lsp/LspService.hpp"
#include <filesystem>
#include <algorithm>
#include <optional>
#include <random>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <set>
#include <sstream>

namespace firmius::audits {
namespace {
using firmius::core::LspRequest;
using namespace firmius::shared;
namespace fs = std::filesystem;

std::string toJson(const rapidjson::Value &value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return buffer.GetString();
}

std::string homeDir() {
  const char *home = std::getenv("HOME");
  return home != nullptr ? home : "/tmp";
}

std::vector<fs::path> collectRepos(const fs::path &cacheDir) {
  std::vector<fs::path> repos;
  if (!fs::exists(cacheDir)) {
    return repos;
  }
  for (const auto &entry : fs::directory_iterator(cacheDir)) {
    if (entry.is_directory()) {
      repos.push_back(entry.path());
    }
  }
  std::sort(repos.begin(), repos.end());
  return repos;
}

std::optional<fs::path> chooseFile(const fs::path &repo) {
  static const std::set<std::string> exts = {".py", ".rs", ".c",  ".cc",
                                             ".cpp", ".cxx", ".go", ".java",
                                             ".ts", ".tsx", ".js", ".jsx"};
  std::vector<fs::path> candidates;
  std::vector<fs::path> preferred;
  for (fs::recursive_directory_iterator it(repo), end; it != end; ++it) {
    const auto &path = it->path();
    if (it->is_directory()) {
      const auto name = path.filename().string();
      if (name == ".git" || name == "node_modules" || name == "build" ||
          name == "dist" || name == ".venv" || name == "venv" ||
          name == "target" || name == "__pycache__") {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file() || !exts.count(path.extension().string())) {
      continue;
    }
    candidates.push_back(path);
    const std::string asString = path.string();
    if (asString.find("/test") == std::string::npos &&
        asString.find("tests/") == std::string::npos) {
      preferred.push_back(path);
    }
  }
  if (!preferred.empty()) {
    return preferred.front();
  }
  if (!candidates.empty()) {
    return candidates.front();
  }
  return std::nullopt;
}

std::string stringMember(const rapidjson::Value &value, const char *key) {
  return value.IsObject() && value.HasMember(key) && value[key].IsString()
             ? value[key].GetString()
             : "";
}

std::optional<std::pair<int, int>> extractPosition(const rapidjson::Document &doc,
                                                   std::string *symbolName) {
  if (!doc.IsObject() || !doc.HasMember("result") || !doc["result"].IsArray() ||
      doc["result"].Empty()) {
    return std::nullopt;
  }
  const auto &first = doc["result"][0];
  if (symbolName != nullptr) {
    *symbolName = stringMember(first, "name");
  }
  if (first.IsObject() && first.HasMember("selectionRange") &&
      first["selectionRange"].IsObject()) {
    const auto &range = first["selectionRange"];
    if (range.HasMember("start") && range["start"].IsObject() &&
        range["start"].HasMember("line") && range["start"]["line"].IsInt() &&
        range["start"].HasMember("character") &&
        range["start"]["character"].IsInt()) {
      return std::make_pair(range["start"]["line"].GetInt() + 1,
                            range["start"]["character"].GetInt() + 1);
    }
  }
  if (first.IsObject() && first.HasMember("location") &&
      first["location"].IsObject()) {
    const auto &location = first["location"];
    if (location.HasMember("range") && location["range"].IsObject()) {
      const auto &range = location["range"];
      if (range.HasMember("start") && range["start"].IsObject() &&
          range["start"].HasMember("line") && range["start"]["line"].IsInt() &&
          range["start"].HasMember("character") &&
          range["start"]["character"].IsInt()) {
        return std::make_pair(range["start"]["line"].GetInt() + 1,
                              range["start"]["character"].GetInt() + 1);
      }
    }
  }
  return std::nullopt;
}

rapidjson::Document runRequest(const LspRequest &request,
                               const std::string &cwd) {
  return firmius::core::runLspRequest(request, cwd);
}

void appendRequestSummary(std::ostringstream &out, const std::string &label,
                          const rapidjson::Document &doc) {
  out << "\n[" << label << "]\n";
  out << "  ok=" << (doc.IsObject() && doc.HasMember("ok") && doc["ok"].IsBool() && doc["ok"].GetBool() ? "true" : "false");
  out << " available="
      << (doc.IsObject() && doc.HasMember("available") && doc["available"].IsBool() && doc["available"].GetBool() ? "true" : "false");
  out << " server=" << (doc.IsObject() ? stringMember(doc, "server_id") : "") << "\n";
  if (doc.IsObject() && doc.HasMember("summary") && doc["summary"].IsObject()) {
    const auto &summary = doc["summary"];
    out << "  errors=" << (summary.HasMember("errors") && summary["errors"].IsInt() ? summary["errors"].GetInt() : 0)
        << " warnings=" << (summary.HasMember("warnings") && summary["warnings"].IsInt() ? summary["warnings"].GetInt() : 0)
        << " files=" << (summary.HasMember("files") && summary["files"].IsInt() ? summary["files"].GetInt() : 0)
        << "\n";
  }
  if (doc.IsObject() && doc.HasMember("error") && doc["error"].IsString()) {
    out << "  error=" << doc["error"].GetString() << "\n";
  }
  if (doc.IsObject() && doc.HasMember("result")) {
    if (doc["result"].IsArray()) {
      out << "  result_count=" << doc["result"].Size() << "\n";
    } else if (doc["result"].IsObject()) {
      out << "  result=" << toJson(doc["result"]) << "\n";
    }
  }
}

} // namespace

std::string LspAudit::getId() const { return "lsp"; }

std::string LspAudit::getDescription() const {
  return "Exercise real LSP diagnostics and semantic calls against a random cached SWE-bench repo";
}

shared::AuditResult LspAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();

  unsigned int seed = static_cast<unsigned int>(std::random_device{}());
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "--seed") {
      seed = static_cast<unsigned int>(std::stoul(args[i + 1]));
    }
  }

  const fs::path cacheDir = fs::path(homeDir()) / ".firmius/cache/swebench/repos";
  auto repos = collectRepos(cacheDir);
  if (repos.empty()) {
    result.exitCode = 1;
    result.passed = false;
    result.output = "No cached SWE-bench repos found at " + cacheDir.string();
    return result;
  }

  std::mt19937 rng(seed);
  std::shuffle(repos.begin(), repos.end(), rng);
  const fs::path repo = repos.front();
  const auto file = chooseFile(repo);
  if (!file.has_value()) {
    result.exitCode = 1;
    result.passed = false;
    result.output = "Could not find a supported source file in " + repo.string();
    return result;
  }


  std::ostringstream out;
  out << "LSP audit seed: " << seed << "\n";
  out << "Repo: " << repo.string() << "\n";
  out << "File: " << file->string() << "\n";

  LspRequest diagFile;
  diagFile.operation = "diagnostics";
  diagFile.path = file->string();
  diagFile.project_root = repo.string();
  diagFile.timeout_ms = 60000;
  const auto diagFileDoc = runRequest(diagFile, repo.string());
  appendRequestSummary(out, "diagnostics:file", diagFileDoc);

  LspRequest diagProject = diagFile;
  diagProject.project = true;
  diagProject.max_files = 25;
  const auto diagProjectDoc = runRequest(diagProject, repo.string());
  appendRequestSummary(out, "diagnostics:project", diagProjectDoc);

  LspRequest docSymbols;
  docSymbols.operation = "document_symbol";
  docSymbols.path = file->string();
  docSymbols.project_root = repo.string();
  docSymbols.timeout_ms = 60000;
  const auto docSymbolsDoc = runRequest(docSymbols, repo.string());
  appendRequestSummary(out, "document_symbol", docSymbolsDoc);

  std::string symbolName = file->stem().string();
  auto position = extractPosition(docSymbolsDoc, &symbolName);
  const int line = position.has_value() ? position->first : 1;
  const int character = position.has_value() ? position->second : 1;

  const std::vector<std::string> ops = {
      "hover",          "definition",         "references",
      "implementation", "prepare_call_hierarchy",
      "incoming_calls", "outgoing_calls",
  };
  int successfulOps = 0;
  for (const auto &op : ops) {
    LspRequest request;
    request.operation = op;
    request.path = file->string();
    request.project_root = repo.string();
    request.line = line;
    request.character = character;
    request.timeout_ms = 60000;
    const auto doc = runRequest(request, repo.string());
    appendRequestSummary(out, op, doc);
    if (doc.IsObject() && doc.HasMember("ok") && doc["ok"].IsBool() &&
        doc["ok"].GetBool() && doc.HasMember("available") &&
        doc["available"].IsBool() && doc["available"].GetBool()) {
      ++successfulOps;
    }
  }

  LspRequest workspaceSymbols;
  workspaceSymbols.operation = "workspace_symbol";
  workspaceSymbols.path = file->string();
  workspaceSymbols.project_root = repo.string();
  workspaceSymbols.query = symbolName;
  workspaceSymbols.timeout_ms = 60000;
  const auto workspaceSymbolsDoc = runRequest(workspaceSymbols, repo.string());
  appendRequestSummary(out, "workspace_symbol", workspaceSymbolsDoc);

  const bool diagnosticsAvailable =
      diagFileDoc.IsObject() && diagFileDoc.HasMember("available") &&
      diagFileDoc["available"].IsBool() && diagFileDoc["available"].GetBool();
  const bool documentSymbolsAvailable =
      docSymbolsDoc.IsObject() && docSymbolsDoc.HasMember("available") &&
      docSymbolsDoc["available"].IsBool() && docSymbolsDoc["available"].GetBool();
  const bool workspaceSymbolsAvailable =
      workspaceSymbolsDoc.IsObject() && workspaceSymbolsDoc.HasMember("available") &&
      workspaceSymbolsDoc["available"].IsBool() && workspaceSymbolsDoc["available"].GetBool();

  result.passed = diagnosticsAvailable && documentSymbolsAvailable &&
                  workspaceSymbolsAvailable && successfulOps >= 4;
  result.exitCode = result.passed ? 0 : 1;
  out << "\nPass criteria: diagnostics + document/workspace symbols available and >=4 position ops executed successfully.\n";
  out << "Result: " << (result.passed ? "PASS" : "FAIL") << "\n";
  result.output = out.str();
  return result;
}

} // namespace firmius::audits
