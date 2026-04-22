#include "audits/LspAudit.hpp"
#include "lsp/LspServerManager.hpp"
#include "lsp/LspService.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <set>
#include <sstream>
#include <vector>

namespace firmius::audits {
namespace {
using firmius::core::LspRequest;
using namespace firmius::shared;
namespace fs = std::filesystem;

struct ParsedArgs {
  unsigned int seed = static_cast<unsigned int>(std::random_device{}());
  fs::path cacheDir = [] {
    const char *home = std::getenv("HOME");
    return fs::path(home != nullptr ? home : "/tmp") / ".firmius/cache/swebench/repos";
  }();
  std::optional<fs::path> explicitRepo;
  size_t sampleCount = 100;
  size_t repoCount = 5;
  int projectMaxFiles = 25;
  int timeoutMs = 60000;
  bool cleanupServers = true;
};

struct TimedLspResponse {
  rapidjson::Document doc;
  double elapsedMs = 0.0;
};

struct DiagnosticsMeasurement {
  fs::path repo;
  fs::path file;
  TimedLspResponse response;
};

struct AggregateStats {
  double minMs = 0.0;
  double avgMs = 0.0;
  double maxMs = 0.0;
  size_t availableCount = 0;
};

struct SemanticTarget {
  fs::path repo;
  fs::path file;
};

std::string toJson(const rapidjson::Value &value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return buffer.GetString();
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

bool isSupportedSourceFile(const fs::path &path) {
  static const std::set<std::string> exts = {".py", ".rs", ".c",  ".cc",
                                             ".cpp", ".cxx", ".go", ".java",
                                             ".ts", ".tsx", ".js", ".jsx"};
  return exts.count(path.extension().string()) > 0;
}

bool isPackagingLikeFile(const fs::path &path) {
  const std::string name = path.filename().string();
  return name == "setup.py" || name == "conftest.py" || name == "__init__.py" ||
         name == "setup_package.py";
}

int semanticFileScore(const fs::path &repo, const fs::path &path) {
  const std::string rel = fs::relative(path, repo).generic_string();
  int score = 0;

  if (rel.find("/docs/") != std::string::npos ||
      rel.find("/tests/") != std::string::npos ||
      rel.find("/testing/") != std::string::npos ||
      rel.find("/scripts/") != std::string::npos) {
    score -= 10;
  }
  if (isPackagingLikeFile(path)) {
    score -= 20;
  }

  const std::string ext = path.extension().string();
  if (ext == ".js" || ext == ".jsx" || ext == ".ts" || ext == ".tsx") {
    score += 40;
  } else if (ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
             ext == ".java" || ext == ".go" || ext == ".rs") {
    score += 25;
  } else if (ext == ".py") {
    score += 5;
  }

  if (ext == ".py") {
    std::ifstream in(path);
    std::string line;
    int structuralLines = 0;
    int importLines = 0;
    int lineBudget = 0;
    while (lineBudget < 250 && std::getline(in, line)) {
      ++lineBudget;
      if (line.rfind("class ", 0) == 0 || line.rfind("def ", 0) == 0) {
        structuralLines += 2;
      } else if (line.rfind("from ", 0) == 0 || line.rfind("import ", 0) == 0) {
        importLines += 1;
      }
    }
    score += structuralLines + importLines;
  } else {
    score += 3;
  }

  if (rel.find("astropy/") == 0 || rel.find("django/") == 0) {
    score += 4;
  }

  return score;
}

std::vector<fs::path> collectCandidateFiles(const fs::path &repo) {
  std::vector<fs::path> preferred;
  std::vector<fs::path> fallback;
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
    if (!it->is_regular_file() || !isSupportedSourceFile(path)) {
      continue;
    }
    const std::string asString = path.string();
    if (asString.find("/test") == std::string::npos &&
        asString.find("tests/") == std::string::npos) {
      preferred.push_back(path);
    } else {
      fallback.push_back(path);
    }
  }

  auto scoreLess = [&](const fs::path &lhs, const fs::path &rhs) {
    const int lhsScore = semanticFileScore(repo, lhs);
    const int rhsScore = semanticFileScore(repo, rhs);
    if (lhsScore != rhsScore) {
      return lhsScore > rhsScore;
    }
    return lhs.generic_string() < rhs.generic_string();
  };

  std::sort(preferred.begin(), preferred.end(), scoreLess);
  std::sort(fallback.begin(), fallback.end(), scoreLess);
  preferred.insert(preferred.end(), fallback.begin(), fallback.end());
  return preferred;
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
        range["start"]["line"].IsInt() && range["start"]["character"].IsInt()) {
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
          range["start"]["line"].IsInt() && range["start"]["character"].IsInt()) {
        return std::make_pair(range["start"]["line"].GetInt() + 1,
                              range["start"]["character"].GetInt() + 1);
      }
    }
  }
  return std::nullopt;
}

TimedLspResponse runRequest(const LspRequest &request, const std::string &cwd) {
  const auto start = std::chrono::steady_clock::now();
  auto doc = firmius::core::runLspRequest(request, cwd);
  const auto end = std::chrono::steady_clock::now();
  TimedLspResponse response;
  response.doc = std::move(doc);
  response.elapsedMs =
      std::chrono::duration<double, std::milli>(end - start).count();
  return response;
}

bool responseSucceeded(const TimedLspResponse &response) {
  return response.doc.IsObject() && response.doc.HasMember("ok") &&
         response.doc["ok"].IsBool() && response.doc["ok"].GetBool() &&
         response.doc.HasMember("available") &&
         response.doc["available"].IsBool() && response.doc["available"].GetBool();
}

void appendRequestSummary(std::ostringstream &out, const std::string &label,
                          const TimedLspResponse &response) {
  const auto &doc = response.doc;
  out << "\n[" << label << "]\n";
  out << "  elapsed_ms=" << response.elapsedMs << "\n";
  out << "  ok=" << (responseSucceeded(response) ? "true" : "false");
  out << " available="
      << (doc.IsObject() && doc.HasMember("available") && doc["available"].IsBool() &&
                  doc["available"].GetBool()
              ? "true"
              : "false");
  out << " server=" << (doc.IsObject() ? stringMember(doc, "server_id") : "") << "\n";
  if (doc.IsObject() && doc.HasMember("summary") && doc["summary"].IsObject()) {
    const auto &summary = doc["summary"];
    out << "  errors="
        << (summary.HasMember("errors") && summary["errors"].IsInt()
                ? summary["errors"].GetInt()
                : 0)
        << " warnings="
        << (summary.HasMember("warnings") && summary["warnings"].IsInt()
                ? summary["warnings"].GetInt()
                : 0)
        << " files="
        << (summary.HasMember("files") && summary["files"].IsInt()
                ? summary["files"].GetInt()
                : 0)
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

bool isAvailable(const rapidjson::Document &doc) {
  return doc.IsObject() && doc.HasMember("available") &&
         doc["available"].IsBool() && doc["available"].GetBool();
}

ParsedArgs parseArgs(const std::vector<std::string> &args, std::string *error) {
  ParsedArgs parsed;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    const auto requireValue = [&](const std::string &flag)
        -> std::optional<std::string> {
      if (i + 1 >= args.size()) {
        if (error != nullptr) {
          *error = "Missing value for " + flag;
        }
        return std::nullopt;
      }
      return args[i + 1];
    };

    if (arg == "--seed") {
      auto value = requireValue(arg);
      if (!value.has_value()) return parsed;
      parsed.seed = static_cast<unsigned int>(std::stoul(*value));
      ++i;
    } else if (arg == "--repo" || arg == "--precloned-repo") {
      auto value = requireValue(arg);
      if (!value.has_value()) return parsed;
      parsed.explicitRepo = fs::path(*value);
      ++i;
    } else if (arg == "--cache-dir") {
      auto value = requireValue(arg);
      if (!value.has_value()) return parsed;
      parsed.cacheDir = fs::path(*value);
      ++i;
    } else if (arg == "--sample-count") {
      auto value = requireValue(arg);
      if (!value.has_value()) return parsed;
      parsed.sampleCount =
          std::max<size_t>(1, static_cast<size_t>(std::stoul(*value)));
      ++i;
    } else if (arg == "--repo-count") {
      auto value = requireValue(arg);
      if (!value.has_value()) return parsed;
      parsed.repoCount =
          std::max<size_t>(1, static_cast<size_t>(std::stoul(*value)));
      ++i;
    } else if (arg == "--project-max-files") {
      auto value = requireValue(arg);
      if (!value.has_value()) return parsed;
      parsed.projectMaxFiles = std::max(1, std::stoi(*value));
      ++i;
    } else if (arg == "--timeout-ms") {
      auto value = requireValue(arg);
      if (!value.has_value()) return parsed;
      parsed.timeoutMs = std::max(1, std::stoi(*value));
      ++i;
    } else if (arg == "--no-cleanup") {
      parsed.cleanupServers = false;
    } else if (arg == "--help" || arg == "-h") {
      if (error != nullptr) {
        *error =
            "Usage: --audit lsp [--repo <precloned_repo>] [--cache-dir <dir>] [--repo-count <n>] [--sample-count <n>] [--project-max-files <n>] [--timeout-ms <ms>] [--seed <n>] [--no-cleanup]";
      }
      return parsed;
    } else {
      if (error != nullptr) {
        *error = "Unknown lsp audit argument: " + arg;
      }
      return parsed;
    }
  }
  return parsed;
}

std::vector<fs::path> resolveRepos(const ParsedArgs &parsed, std::mt19937 &rng,
                                   std::string *error) {
  if (parsed.explicitRepo.has_value()) {
    const fs::path repo = fs::absolute(*parsed.explicitRepo);
    if (!fs::exists(repo) || !fs::is_directory(repo)) {
      if (error != nullptr) {
        *error = "Explicit repo path does not exist: " + repo.string();
      }
      return {};
    }
    return {repo};
  }

  auto repos = collectRepos(parsed.cacheDir);
  if (repos.empty()) {
    if (error != nullptr) {
      *error = "No cached SWE-bench repos found at " + parsed.cacheDir.string();
    }
    return {};
  }
  std::shuffle(repos.begin(), repos.end(), rng);
  if (parsed.repoCount < repos.size()) {
    repos.resize(parsed.repoCount);
  }
  std::sort(repos.begin(), repos.end());
  return repos;
}

AggregateStats computeAggregateStats(
    const std::vector<DiagnosticsMeasurement> &measurements) {
  AggregateStats stats;
  if (measurements.empty()) {
    return stats;
  }
  double totalMs = 0.0;
  stats.minMs = std::numeric_limits<double>::max();
  stats.maxMs = 0.0;
  for (const auto &measurement : measurements) {
    totalMs += measurement.response.elapsedMs;
    stats.minMs = std::min(stats.minMs, measurement.response.elapsedMs);
    stats.maxMs = std::max(stats.maxMs, measurement.response.elapsedMs);
    if (isAvailable(measurement.response.doc)) {
      ++stats.availableCount;
    }
  }
  stats.avgMs = totalMs / static_cast<double>(measurements.size());
  return stats;
}

void appendDiagnosticsTimingSummary(
    std::ostringstream &out,
    const std::vector<DiagnosticsMeasurement> &measurements) {
  out << "\n[random_diagnostics]\n";
  if (measurements.empty()) {
    out << "  sample_count=0\n";
    return;
  }

  const auto stats = computeAggregateStats(measurements);
  for (size_t i = 0; i < measurements.size(); ++i) {
    out << "  sample[" << i << "] repo=" << measurements[i].repo.string()
        << " file=" << measurements[i].file.string()
        << " elapsed_ms=" << measurements[i].response.elapsedMs
        << " available="
        << (isAvailable(measurements[i].response.doc) ? "true" : "false")
        << "\n";
  }
  out << "  sample_count=" << measurements.size() << "\n";
  out << "  available_count=" << stats.availableCount << "\n";
  out << "  min_ms=" << stats.minMs << "\n";
  out << "  avg_ms=" << stats.avgMs << "\n";
  out << "  max_ms=" << stats.maxMs << "\n";
}

std::vector<DiagnosticsMeasurement> measureRandomDiagnostics(
    const std::vector<fs::path> &repos, std::mt19937 &rng,
    const ParsedArgs &parsed) {
  std::vector<DiagnosticsMeasurement> measurements;
  if (repos.empty() || parsed.sampleCount == 0) {
    return measurements;
  }

  const size_t targetSamples = parsed.sampleCount;
  const size_t basePerRepo = std::max<size_t>(1, targetSamples / repos.size());
  size_t remainder = targetSamples % repos.size();

  for (size_t repoIndex = 0; repoIndex < repos.size(); ++repoIndex) {
    auto candidates = collectCandidateFiles(repos[repoIndex]);
    if (candidates.empty()) {
      continue;
    }
    std::shuffle(candidates.begin(), candidates.end(), rng);
    const size_t takeCount =
        std::min(candidates.size(), basePerRepo + (remainder > 0 ? 1 : 0));
    if (remainder > 0) {
      --remainder;
    }
    for (size_t fileIndex = 0;
         fileIndex < takeCount && measurements.size() < targetSamples;
         ++fileIndex) {
      LspRequest sampleDiag;
      sampleDiag.operation = "diagnostics";
      sampleDiag.path = candidates[fileIndex].string();
      sampleDiag.project_root = repos[repoIndex].string();
      sampleDiag.timeout_ms = parsed.timeoutMs;
      DiagnosticsMeasurement measurement;
      measurement.repo = repos[repoIndex];
      measurement.file = candidates[fileIndex];
      measurement.response =
          runRequest(sampleDiag, repos[repoIndex].string());
      measurements.push_back(std::move(measurement));
    }
    if (parsed.cleanupServers) {
      firmius::core::LspServerManager::instance().shutdownAll();
    }
  }
  return measurements;
}

void cleanupServers(bool cleanupServers) {
  if (cleanupServers) {
    firmius::core::LspServerManager::instance().shutdownAll();
  }
}

std::optional<SemanticTarget> chooseSemanticTarget(
    const std::vector<fs::path> &repos, const ParsedArgs &parsed) {
  for (const auto &repo : repos) {
    const auto candidates = collectCandidateFiles(repo);
    size_t tried = 0;
    for (const auto &candidate : candidates) {
      if (isPackagingLikeFile(candidate)) {
        continue;
      }
      LspRequest docSymbols;
      docSymbols.operation = "document_symbol";
      docSymbols.path = candidate.string();
      docSymbols.project_root = repo.string();
      docSymbols.timeout_ms = std::min(parsed.timeoutMs, 4000);
      const auto docSymbolsDoc = runRequest(docSymbols, repo.string());
      if (!responseSucceeded(docSymbolsDoc)) {
        cleanupServers(parsed.cleanupServers);
        ++tried;
        if (tried >= 20) break;
        continue;
      }

      std::string symbolName = candidate.stem().string();
      const auto position = extractPosition(docSymbolsDoc.doc, &symbolName);
      const int line = position.has_value() ? position->first : 1;
      const int character = position.has_value() ? position->second : 1;

      LspRequest hover;
      hover.operation = "hover";
      hover.path = candidate.string();
      hover.project_root = repo.string();
      hover.line = line;
      hover.character = character;
      hover.timeout_ms = std::min(parsed.timeoutMs, 4000);
      const auto hoverDoc = runRequest(hover, repo.string());
      cleanupServers(parsed.cleanupServers);
      if (responseSucceeded(hoverDoc)) {
        return SemanticTarget{repo, candidate};
      }
      ++tried;
      if (tried >= 20) break;
    }
  }
  return std::nullopt;
}

} // namespace

std::string LspAudit::getId() const { return "lsp"; }

std::string LspAudit::getDescription() const {
  return "Exercise real LSP diagnostics and semantic calls against pre-cloned or cached SWE-bench repos with timing output and cleanup";
}

shared::AuditResult LspAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();

  std::string argError;
  const ParsedArgs parsed = parseArgs(args, &argError);
  if (!argError.empty()) {
    result.exitCode = 1;
    result.passed = false;
    result.output = argError;
    return result;
  }

  std::mt19937 rng(parsed.seed);
  std::string repoError;
  const auto repos = resolveRepos(parsed, rng, &repoError);
  if (repos.empty()) {
    result.exitCode = 1;
    result.passed = false;
    result.output = repoError;
    return result;
  }

  const auto randomMeasurements = measureRandomDiagnostics(repos, rng, parsed);
  if (randomMeasurements.empty()) {
    result.exitCode = 1;
    result.passed = false;
    result.output =
        "Could not find random source file samples across selected repos.";
    return result;
  }

  const auto semanticTarget = chooseSemanticTarget(repos, parsed);
  if (!semanticTarget.has_value()) {
    result.exitCode = 1;
    result.passed = false;
    result.output =
        "Could not find a semantic-friendly source file across selected repos.";
    return result;
  }

  const fs::path semanticRepo = semanticTarget->repo;
  const fs::path semanticFile = semanticTarget->file;

  std::ostringstream out;
  out << "LSP audit seed: " << parsed.seed << "\n";
  out << "Repo count: " << repos.size() << "\n";
  out << "Repos:\n";
  for (const auto &repo : repos) {
    out << "  - " << repo.string() << "\n";
  }
  out << "Semantic repo: " << semanticRepo.string() << "\n";
  out << "Primary file: " << semanticFile.string() << "\n";
  out << "Random diagnostics sample count: " << randomMeasurements.size()
      << "\n";
  out << "Cleanup servers: "
      << (parsed.cleanupServers ? "true" : "false") << "\n";

  appendDiagnosticsTimingSummary(out, randomMeasurements);

  LspRequest diagFile;
  diagFile.operation = "diagnostics";
  diagFile.path = semanticFile.string();
  diagFile.project_root = semanticRepo.string();
  diagFile.timeout_ms = parsed.timeoutMs;
  const auto diagFileDoc = runRequest(diagFile, semanticRepo.string());
  appendRequestSummary(out, "diagnostics:file", diagFileDoc);

  LspRequest diagProject = diagFile;
  diagProject.project = true;
  diagProject.max_files = parsed.projectMaxFiles;
  const auto diagProjectDoc = runRequest(diagProject, semanticRepo.string());
  appendRequestSummary(out, "diagnostics:project", diagProjectDoc);

  LspRequest docSymbols;
  docSymbols.operation = "document_symbol";
  docSymbols.path = semanticFile.string();
  docSymbols.project_root = semanticRepo.string();
  docSymbols.timeout_ms = parsed.timeoutMs;
  const auto docSymbolsDoc = runRequest(docSymbols, semanticRepo.string());
  appendRequestSummary(out, "document_symbol", docSymbolsDoc);
  if (!responseSucceeded(docSymbolsDoc)) {
    cleanupServers(parsed.cleanupServers);
    result.exitCode = 1;
    result.passed = false;
    out << "\nActive servers after cleanup: "
        << firmius::core::LspServerManager::instance().activeServerCount()
        << "\n";
    out << "Result: FAIL\n";
    result.output = out.str();
    return result;
  }

  std::string symbolName = semanticFile.stem().string();
  const auto position = extractPosition(docSymbolsDoc.doc, &symbolName);
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
    request.path = semanticFile.string();
    request.project_root = semanticRepo.string();
    request.line = line;
    request.character = character;
    request.timeout_ms = parsed.timeoutMs;
    const auto doc = runRequest(request, semanticRepo.string());
    appendRequestSummary(out, op, doc);
    if (responseSucceeded(doc)) {
      ++successfulOps;
    }
  }

  LspRequest workspaceSymbols;
  workspaceSymbols.operation = "workspace_symbol";
  workspaceSymbols.path = semanticFile.string();
  workspaceSymbols.project_root = semanticRepo.string();
  workspaceSymbols.query = symbolName;
  workspaceSymbols.timeout_ms = parsed.timeoutMs;
  const auto workspaceSymbolsDoc =
      runRequest(workspaceSymbols, semanticRepo.string());
  appendRequestSummary(out, "workspace_symbol", workspaceSymbolsDoc);

  cleanupServers(parsed.cleanupServers);

  const bool diagnosticsAvailable = isAvailable(diagFileDoc.doc);
  const bool randomDiagnosticsAvailable =
      std::all_of(randomMeasurements.begin(), randomMeasurements.end(),
                  [](const DiagnosticsMeasurement &measurement) {
                    return isAvailable(measurement.response.doc);
                  });
  const bool documentSymbolsAvailable = isAvailable(docSymbolsDoc.doc);
  const bool workspaceSymbolsAvailable = isAvailable(workspaceSymbolsDoc.doc);

  result.passed = diagnosticsAvailable && randomDiagnosticsAvailable &&
                  documentSymbolsAvailable && workspaceSymbolsAvailable &&
                  successfulOps >= 4;
  result.exitCode = result.passed ? 0 : 1;
  out << "\nPass criteria: random file diagnostics available for all samples, primary diagnostics available, document/workspace symbols available, and >=4 position ops executed successfully.\n";
  out << "Active servers after cleanup: "
      << firmius::core::LspServerManager::instance().activeServerCount()
      << "\n";
  out << "Result: " << (result.passed ? "PASS" : "FAIL") << "\n";
  result.output = out.str();
  return result;
}

} // namespace firmius::audits
