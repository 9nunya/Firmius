#include "workflow/WorkflowLoader.hpp"
#include "utils/FrontmatterParser.hpp"
#include "utils/PlatformPaths.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <cctype>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace firmius::core {

namespace {

bool ensureWritableDirectory(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec || !std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
    return false;
  }

  const auto probe = dir / (".write_probe_" + firmius::shared::StringUtil::generateUuid());
  std::ofstream out(probe);
  if (!out.is_open()) {
    return false;
  }
  out << "ok";
  out.close();
  std::filesystem::remove(probe, ec);
  return true;
}

bool isUsableWorkflowDir(const std::filesystem::path &dir) {
  try {
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
      return false;
    }
    // A directory is "usable" only once it contains at least one readable
    // workflow file. An empty directory should be treated as uninitialised
    // so bootstrapDefaults can seed built-in workflows into it.
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".md") {
        continue;
      }
      std::ifstream file(entry.path());
      if (file.good()) {
        return true;
      }
    }
    return false;
  } catch (...) {
    return false;
  }
}

std::filesystem::path resolveWritableFirmiusHome() {
  const std::filesystem::path userHome = firmius::shared::PlatformPaths::firmiusHomeDir();
  if (ensureWritableDirectory(userHome)) {
    return userHome;
  }

  const std::filesystem::path localHome =
      std::filesystem::current_path() / ".firmius";
  if (ensureWritableDirectory(localHome)) {
    return localHome;
  }

  const std::filesystem::path tempHome =
      std::filesystem::temp_directory_path() /
      ("firmius-" + std::to_string(static_cast<long long>(
#if defined(_WIN32)
          GetCurrentProcessId()
#else
          getuid()
#endif
      )));
  if (ensureWritableDirectory(tempHome)) {
    return tempHome;
  }

  return std::filesystem::temp_directory_path() / "firmius";
}

std::string ensureTrailingSlash(std::string dir) {
  if (!dir.empty() && dir.back() != '/') {
    dir += '/';
  }
  return dir;
}

std::string trimCopy(std::string value) {
  auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
              value.end());
  return value;
}

std::vector<std::filesystem::path>
parsePackManifestFiles(const std::filesystem::path &packManifest) {
  std::ifstream in(packManifest);
  if (!in.is_open()) {
    return {};
  }

  std::vector<std::filesystem::path> files;
  bool inFiles = false;
  std::string line;
  while (std::getline(in, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    if (trimCopy(line).empty()) {
      continue;
    }

    if (!line.empty() && !std::isspace(static_cast<unsigned char>(line[0]))) {
      const auto trimmed = trimCopy(line);
      if (trimmed == "files:") {
        inFiles = true;
        continue;
      }
      if (inFiles) {
        break;
      }
    }

    if (!inFiles) {
      continue;
    }

    const auto trimmed = trimCopy(line);
    if (trimmed.rfind("- ", 0) != 0) {
      continue;
    }
    auto relative = trimCopy(trimmed.substr(2));
    if ((relative.size() >= 2 && relative.front() == '"' &&
         relative.back() == '"') ||
        (relative.size() >= 2 && relative.front() == '\'' &&
         relative.back() == '\'')) {
      relative = relative.substr(1, relative.size() - 2);
    }
    if (!relative.empty()) {
      files.emplace_back(relative);
    }
  }

  return files;
}

std::string parsePackManifestId(const std::filesystem::path &packManifest) {
  std::ifstream in(packManifest);
  if (!in.is_open()) {
    return {};
  }

  std::string line;
  while (std::getline(in, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    const auto trimmed = trimCopy(line);
    if (trimmed.rfind("id:", 0) != 0) {
      continue;
    }
    auto id = trimCopy(trimmed.substr(3));
    if ((id.size() >= 2 && id.front() == '"' && id.back() == '"') ||
        (id.size() >= 2 && id.front() == '\'' && id.back() == '\'')) {
      id = id.substr(1, id.size() - 2);
    }
    return id;
  }

  return {};
}

struct ParsedPackManifest {
  std::string id;
  std::vector<std::filesystem::path> files;
  WorkflowPackStateSurface stateSurface;
};

ParsedPackManifest parsePackManifest(const std::filesystem::path &packManifest) {
  ParsedPackManifest parsed;
  parsed.id = parsePackManifestId(packManifest);
  parsed.files = parsePackManifestFiles(packManifest);

  std::ifstream in(packManifest);
  if (!in.is_open()) {
    return parsed;
  }

  enum class Section { None, StateSurface, Scopes, Paths };
  Section section = Section::None;
  std::string line;
  while (std::getline(in, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    if (trimCopy(line).empty()) {
      continue;
    }

    const bool rootLevel =
        !line.empty() && !std::isspace(static_cast<unsigned char>(line[0]));
    const std::string trimmed = trimCopy(line);
    if (rootLevel) {
      if (trimmed == "state_surface:") {
        section = Section::StateSurface;
        continue;
      }
      if (section != Section::None) {
        section = Section::None;
      }
    }

    if (section == Section::None) {
      continue;
    }

    if (section == Section::StateSurface) {
      if (trimmed == "scopes:") {
        section = Section::Scopes;
        continue;
      }
      if (trimmed == "paths:") {
        section = Section::Paths;
        continue;
      }
      if (trimmed.rfind("scopes:", 0) == 0) {
        auto value = trimCopy(trimmed.substr(7));
        if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
          value = value.substr(1, value.size() - 2);
          std::stringstream ss(value);
          std::string item;
          while (std::getline(ss, item, ',')) {
            item = trimCopy(item);
            if (!item.empty()) {
              parsed.stateSurface.scopes.push_back(item);
            }
          }
        }
        continue;
      }
    }

    if (trimmed.rfind("- ", 0) != 0) {
      if (section == Section::Scopes || section == Section::Paths) {
        section = Section::StateSurface;
      }
      continue;
    }

    std::string value = trimCopy(trimmed.substr(2));
    if ((value.size() >= 2 && value.front() == '"' && value.back() == '"') ||
        (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')) {
      value = value.substr(1, value.size() - 2);
    }
    if (value.empty()) {
      continue;
    }
    if (section == Section::Scopes) {
      parsed.stateSurface.scopes.push_back(value);
    } else if (section == Section::Paths) {
      parsed.stateSurface.paths.push_back(value);
    }
  }

  return parsed;
}

void applyPackMetadata(Workflow &workflow, const ParsedPackManifest &manifest) {
  workflow.packId = manifest.id;
  if (!manifest.stateSurface.scopes.empty() ||
      !manifest.stateSurface.paths.empty()) {
    workflow.packStateSurface = manifest.stateSurface;
  }
}

} // namespace


WorkflowLoader &WorkflowLoader::instance() {
  static WorkflowLoader inst;
  return inst;
}

void WorkflowLoader::init() {
  workflows_.clear();
  std::string dir = getWorkflowsDir();

  if (std::filesystem::exists(dir)) {
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".md") {
        auto workflow = loadWorkflow(entry.path().string());
        if (workflow) {
          workflows_[workflow->id] = *workflow;
        }
      }
    }
  }

  loadHookPacks();
}

void WorkflowLoader::loadHookPacks() {
  auto loadHookFile = [this](const std::filesystem::path &path,
                             const ParsedPackManifest *manifest = nullptr) {
    if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
      return;
    }
    const auto ext = path.extension().string();
    if (ext == ".md") {
      auto workflow = loadWorkflow(path.string());
      if (workflow) {
        if (manifest != nullptr) {
          applyPackMetadata(*workflow, *manifest);
        }
        workflows_[workflow->id] = *workflow;
      }
    } else if (ext == ".yaml" || ext == ".yml") {
      auto workflow = loadYamlWorkflow(path.string());
      if (workflow) {
        if (manifest != nullptr) {
          applyPackMetadata(*workflow, *manifest);
        }
        workflows_[workflow->id] = *workflow;
      }
    }
  };

  std::set<std::string> claimedPackIds;
  for (const auto &dir : getHookDirs()) {
    const std::filesystem::path dirPath(dir);
    if (!std::filesystem::exists(dirPath) ||
        !std::filesystem::is_directory(dirPath)) {
      continue;
    }

    std::set<std::filesystem::path> manifestRoots;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(dirPath)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto filename = entry.path().filename().string();
      if (filename != "pack.yaml" && filename != "pack.yml") {
        continue;
      }
      const auto packRoot = entry.path().parent_path();
      const auto parsedManifest = parsePackManifest(entry.path());
      const auto &packId = parsedManifest.id;
      if (!packId.empty() && claimedPackIds.count(packId) > 0) {
        manifestRoots.insert(std::filesystem::weakly_canonical(packRoot));
        continue;
      }

      if (parsedManifest.files.empty()) {
        continue;
      }

      if (!packId.empty()) {
        claimedPackIds.insert(packId);
      }
      manifestRoots.insert(std::filesystem::weakly_canonical(packRoot));
      for (const auto &relative : parsedManifest.files) {
        loadHookFile(packRoot / relative, &parsedManifest);
      }
    }

    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(dirPath)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto canonicalPath = std::filesystem::weakly_canonical(entry.path());
      bool insideManifestPack = false;
      for (const auto &root : manifestRoots) {
        auto rel = canonicalPath.lexically_relative(root);
        if (!rel.empty() && rel.string().find("..") != 0) {
          insideManifestPack = true;
          break;
        }
      }
      if (insideManifestPack) {
        continue;
      }
      loadHookFile(entry.path());
    }
  }
}

const Workflow *WorkflowLoader::getWorkflow(const std::string &id) const {
  auto it = workflows_.find(id);
  if (it != workflows_.end()) {
    return &it->second;
  }
  return nullptr;
}

std::vector<Workflow> WorkflowLoader::getAllWorkflows() const {
  std::vector<Workflow> result;
  result.reserve(workflows_.size());
  for (const auto &[id, workflow] : workflows_) {
    result.push_back(workflow);
  }
  return result;
}

std::vector<std::string> WorkflowLoader::getWorkflowIds() const {
  std::vector<std::string> result;
  result.reserve(workflows_.size());
  for (const auto &[id, workflow] : workflows_) {
    result.push_back(id);
  }
  return result;
}

void WorkflowLoader::bootstrapDefaults(const std::string &builtinWorkflowsDir) {
  if (!std::filesystem::exists(builtinWorkflowsDir))
    return;

  const std::filesystem::path userDir = resolveWritableFirmiusHome() / "workflows";
  if (isUsableWorkflowDir(userDir)) {
    return;
  }

  try {
    std::filesystem::create_directories(userDir);
  } catch (const std::filesystem::filesystem_error &) {
    return;
  }

  for (const auto &entry :
       std::filesystem::directory_iterator(builtinWorkflowsDir)) {
    if (entry.is_regular_file()) {
      try {
        std::filesystem::copy_file(
            entry.path(), userDir / entry.path().filename(),
            std::filesystem::copy_options::overwrite_existing);
      } catch (const std::filesystem::filesystem_error &) {
      }
    }
  }
}

std::optional<Workflow> WorkflowLoader::loadWorkflow(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "Warning: Could not open workflow file: " << path << std::endl;
    return std::nullopt;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  firmius::shared::FrontmatterDocument doc;
  try {
    const std::string ext = std::filesystem::path(path).extension().string();
    if (ext == ".yaml" || ext == ".yml") {
      doc.values = firmius::shared::FrontmatterParser::parse(content, path);
      doc.body.clear();
    } else {
      doc = firmius::shared::FrontmatterParser::parseMarkdown(content, path);
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: Failed to parse workflow frontmatter in " << path
              << ": " << e.what() << std::endl;
    return std::nullopt;
  }

  Workflow workflow;
  std::filesystem::path fsPath(path);
  workflow.id = fsPath.stem().string();
  workflow.sourcePath = fsPath.string();
  workflow.sourceDir = fsPath.parent_path().string();
  if (auto explicitId = firmius::shared::FrontmatterParser::getString(doc, "id")) {
    if (!firmius::shared::StringUtil::trim(*explicitId).empty()) {
      workflow.id = *explicitId;
    }
  }
  workflow.name = firmius::shared::FrontmatterParser::getString(doc, "name").value_or("");
  workflow.description = firmius::shared::FrontmatterParser::getString(doc, "description").value_or("");
  workflow.body = firmius::shared::StringUtil::trim(doc.body);

  // ── Day-2 unified fields ────────────────────────────────────────────────
  // slash_command, trigger, action, returns, guard. All optional. Existing
  // workflow files continue to load with default-constructed defaults.
  if (auto cmd = firmius::shared::FrontmatterParser::getString(doc, "slash_command")) {
    if (!firmius::shared::StringUtil::trim(*cmd).empty()) {
      workflow.slashCommand = *cmd;
    }
  }
  if (auto raw = firmius::shared::FrontmatterParser::getBool(doc, "raw_remainder")) {
    workflow.rawRemainder = *raw;
  }

  auto extractStringField =
      [](const firmius::shared::FrontmatterValue::Map &m,
         const std::string &key) -> std::optional<std::string> {
    auto it = m.find(key);
    if (it == m.end()) {
      return std::nullopt;
    }
    if (const auto *s = std::get_if<std::string>(&it->second.value)) {
      return *s;
    }
    return std::nullopt;
  };
  auto extractBoolField =
      [](const firmius::shared::FrontmatterValue::Map &m,
         const std::string &key) -> std::optional<bool> {
    auto it = m.find(key);
    if (it == m.end()) {
      return std::nullopt;
    }
    if (const auto *b = std::get_if<bool>(&it->second.value)) {
      return *b;
    }
    if (const auto *s = std::get_if<std::string>(&it->second.value)) {
      return *s == "true" || *s == "yes" || *s == "1";
    }
    if (const auto *i = std::get_if<int64_t>(&it->second.value)) {
      return *i != 0;
    }
    return std::nullopt;
  };
  auto extractIntField =
      [](const firmius::shared::FrontmatterValue::Map &m,
         const std::string &key) -> std::optional<int> {
    auto it = m.find(key);
    if (it == m.end()) {
      return std::nullopt;
    }
    if (const auto *i = std::get_if<int64_t>(&it->second.value)) {
      return static_cast<int>(*i);
    }
    if (const auto *s = std::get_if<std::string>(&it->second.value)) {
      try {
        return std::stoi(*s);
      } catch (...) {
        return std::nullopt;
      }
    }
    return std::nullopt;
  };

  if (auto trig = firmius::shared::FrontmatterParser::getMap(doc, "trigger")) {
    if (auto eventName = extractStringField(*trig, "on_event")) {
      workflow.trigger.kind = WorkflowTrigger::Kind::OnEvent;
      workflow.trigger.event = workflowEventKindFromString(*eventName);
    }
    if (auto matchIt = trig->find("match"); matchIt != trig->end()) {
      if (const auto *matchMap =
              std::get_if<firmius::shared::FrontmatterValue::Map>(
                  &matchIt->second.value)) {
        for (const auto &[k, v] : *matchMap) {
          // Back-compat: primitive values mean equals(k == value).
          if (const auto *s = std::get_if<std::string>(&v.value)) {
            workflow.trigger.match.equals[k] = *s;
            continue;
          }
          if (const auto *b = std::get_if<bool>(&v.value)) {
            workflow.trigger.match.equals[k] = *b ? "true" : "false";
            continue;
          }
          if (const auto *i = std::get_if<int64_t>(&v.value)) {
            workflow.trigger.match.equals[k] = std::to_string(*i);
            continue;
          }

          // Structured predicate: { equals: <primitive> } or { present: <bool> }
          if (const auto *predMap =
                  std::get_if<firmius::shared::FrontmatterValue::Map>(&v.value)) {
            if (auto it = predMap->find("equals"); it != predMap->end()) {
              if (const auto *es = std::get_if<std::string>(&it->second.value)) {
                workflow.trigger.match.equals[k] = *es;
              } else if (const auto *eb =
                             std::get_if<bool>(&it->second.value)) {
                workflow.trigger.match.equals[k] = *eb ? "true" : "false";
              } else if (const auto *ei =
                             std::get_if<int64_t>(&it->second.value)) {
                workflow.trigger.match.equals[k] = std::to_string(*ei);
              }
            }
            if (auto it = predMap->find("present"); it != predMap->end()) {
              if (const auto *pb = std::get_if<bool>(&it->second.value)) {
                workflow.trigger.match.present[k] = *pb;
              }
            }
          }
        }
      }
    }
    if (auto block = extractBoolField(*trig, "block")) {
      workflow.trigger.block = *block;
    }
  }

  if (auto act = firmius::shared::FrontmatterParser::getMap(doc, "action")) {
    if (auto kindStr = extractStringField(*act, "kind")) {
      workflow.action.kind = workflowActionKindFromString(*kindStr);
    }
    if (auto cmd = extractStringField(*act, "command")) {
      workflow.action.command = *cmd;
    }
    if (auto target = extractStringField(*act, "target_workflow")) {
      workflow.action.targetWorkflow = *target;
    }
    if (auto targetArgs = act->find("target_args"); targetArgs != act->end()) {
      if (const auto *arr =
              std::get_if<firmius::shared::FrontmatterValue::Array>(
                  &targetArgs->second.value)) {
        for (const auto &elem : *arr) {
          if (const auto *s = std::get_if<std::string>(&elem.value)) {
            workflow.action.targetArgs.push_back(*s);
          }
        }
      }
    }
    if (auto persona = extractStringField(*act, "target_persona")) {
      workflow.action.targetPersona = *persona;
    }
    if (auto task = extractStringField(*act, "agent_task")) {
      workflow.action.agentTask = *task;
    }
    if (auto t = extractIntField(*act, "timeout_s")) {
      workflow.action.timeoutSec = *t;
    }
    if (auto inj = extractBoolField(*act, "inject_stdout")) {
      workflow.action.injectStdout = *inj;
    }
    if (auto inj = extractBoolField(*act, "inject_stderr_on_fail")) {
      workflow.action.injectStderrOnFail = *inj;
    }

    // ─── Hook-platform action sub-fields ──────────────────────────────────
    // Shell: stdin envelope + Claude Code / opencode compat shim.
    if (auto pe = extractBoolField(*act, "pass_envelope")) {
      workflow.action.passEnvelope = *pe;
    }
    if (auto cc = extractBoolField(*act, "claude_code_compat")) {
      workflow.action.claudeCodeCompat = *cc;
    }
    // Agent: initial mode + expected trophy schema.
    if (auto im = extractStringField(*act, "initial_mode")) {
      workflow.action.initialMode = *im;
    }
    if (auto rs = extractStringField(*act, "return_schema")) {
      workflow.action.returnSchema = *rs;
    }
    // Script: language + inline body.
    if (auto lang = extractStringField(*act, "language")) {
      workflow.action.scriptLanguage = *lang;
    }
    if (auto body = extractStringField(*act, "body")) {
      workflow.action.scriptBody = *body;
    }
    if (auto file = extractStringField(*act, "script_file")) {
      workflow.action.scriptFile = *file;
    }
    if (auto file = extractStringField(*act, "file")) {
      workflow.action.scriptFile = *file;
    }
    // State writes: array of { scope, path, value, append? }.
    auto parseStateWrites =
        [&](const firmius::shared::FrontmatterValue::Array &arr,
            std::vector<WorkflowStateWrite> &dest) {
          for (const auto &elem : arr) {
            const auto *m =
                std::get_if<firmius::shared::FrontmatterValue::Map>(
                    &elem.value);
            if (m == nullptr) continue;
            WorkflowStateWrite sw;
            if (auto s = extractStringField(*m, "scope")) sw.scope = *s;
            if (auto s = extractStringField(*m, "path"))  sw.path = *s;
            if (auto s = extractStringField(*m, "value")) sw.valueTemplate = *s;
            if (auto b = extractBoolField(*m, "append"))  sw.append = *b;
            // Trailing [] in the path implies append even without explicit flag.
            if (!sw.path.empty() && sw.path.size() >= 2 &&
                sw.path[sw.path.size() - 2] == '[' &&
                sw.path[sw.path.size() - 1] == ']') {
              sw.append = true;
            }
            if (!sw.scope.empty() && !sw.path.empty()) {
              dest.push_back(std::move(sw));
            }
          }
        };
    if (auto sw = act->find("writes"); sw != act->end()) {
      if (const auto *arr =
              std::get_if<firmius::shared::FrontmatterValue::Array>(
                  &sw->second.value)) {
        parseStateWrites(*arr, workflow.action.stateWrites);
      }
    }
    if (auto sw = act->find("state_writes"); sw != act->end()) {
      if (const auto *arr =
              std::get_if<firmius::shared::FrontmatterValue::Array>(
                  &sw->second.value)) {
        parseStateWrites(*arr, workflow.action.stateWrites);
      }
    }
    // Compose: array of step maps. Flat-params for now; nested action
    // bodies and per-step state_writes land in a follow-up.
    if (auto cs = act->find("steps"); cs != act->end()) {
      if (const auto *arr =
              std::get_if<firmius::shared::FrontmatterValue::Array>(
                  &cs->second.value)) {
        for (const auto &elem : *arr) {
          const auto *m =
              std::get_if<firmius::shared::FrontmatterValue::Map>(
                  &elem.value);
          if (m == nullptr) continue;
          WorkflowComposeStep step;
          if (auto k = extractStringField(*m, "kind")) step.kind = *k;
          if (auto b = extractStringField(*m, "body")) step.body = *b;
          for (const auto &[k, v] : *m) {
            if (k == "kind" || k == "body") continue;
            if (const auto *s = std::get_if<std::string>(&v.value)) {
              step.params[k] = *s;
            } else if (const auto *b = std::get_if<bool>(&v.value)) {
              step.params[k] = *b ? "true" : "false";
            } else if (const auto *i = std::get_if<int64_t>(&v.value)) {
              step.params[k] = std::to_string(*i);
            }
          }
          if (!step.kind.empty()) {
            workflow.action.composeSteps.push_back(std::move(step));
          }
        }
      }
    }
  } else {
    // No explicit action specified: legacy workflows imply a Prompt action
    // whose body is the markdown body. Already represented by defaults.
  }

  // ─── Hook state surface declaration ──────────────────────────────────────
  if (auto st = firmius::shared::FrontmatterParser::getMap(doc, "state")) {
    WorkflowHookState hs;
    if (auto s = extractStringField(*st, "scope")) hs.scope = *s;
    auto pullArr = [&](const char *key, std::vector<std::string> &dest) {
      auto it = st->find(key);
      if (it == st->end()) return;
      const auto *arr =
          std::get_if<firmius::shared::FrontmatterValue::Array>(
              &it->second.value);
      if (arr == nullptr) return;
      for (const auto &elem : *arr) {
        if (const auto *s = std::get_if<std::string>(&elem.value)) {
          dest.push_back(*s);
        }
      }
    };
    pullArr("reads", hs.reads);
    pullArr("writes", hs.writes);
    if (!hs.scope.empty() || !hs.reads.empty() || !hs.writes.empty()) {
      workflow.hookState = std::move(hs);
    }
  }

  // ─── Emit channel (outcome label, post-action state writes,
  //     block decision) ────────────────────────────────────────────────────
  if (auto em = firmius::shared::FrontmatterParser::getMap(doc, "emit")) {
    WorkflowEmit emit;
    if (auto o = extractStringField(*em, "outcome")) emit.outcomeTemplate = *o;
    if (auto sw = em->find("state_writes"); sw != em->end()) {
      if (const auto *arr =
              std::get_if<firmius::shared::FrontmatterValue::Array>(
                  &sw->second.value)) {
        for (const auto &elem : *arr) {
          const auto *m =
              std::get_if<firmius::shared::FrontmatterValue::Map>(
                  &elem.value);
          if (m == nullptr) continue;
          WorkflowStateWrite swr;
          if (auto s = extractStringField(*m, "scope")) swr.scope = *s;
          if (auto s = extractStringField(*m, "path"))  swr.path = *s;
          if (auto s = extractStringField(*m, "value")) swr.valueTemplate = *s;
          if (auto b = extractBoolField(*m, "append"))  swr.append = *b;
          if (!swr.scope.empty() && !swr.path.empty()) {
            emit.stateWrites.push_back(std::move(swr));
          }
        }
      }
    }
    if (auto bd = em->find("block_decision"); bd != em->end()) {
      if (const auto *m =
              std::get_if<firmius::shared::FrontmatterValue::Map>(
                  &bd->second.value)) {
        WorkflowBlockDecision dec;
        if (auto s = extractStringField(*m, "if"))             dec.condition = *s;
        if (auto s = extractStringField(*m, "then"))           dec.thenBranch = *s;
        if (auto s = extractStringField(*m, "else"))           dec.elseBranch = *s;
        if (auto s = extractStringField(*m, "inject_to_agent")) dec.injectToAgent = *s;
        if (!dec.condition.empty() || !dec.thenBranch.empty() ||
            !dec.elseBranch.empty()) {
          emit.blockDecision = std::move(dec);
        }
      }
    }
    workflow.emit = std::move(emit);
  }

  // ─── Tool-defining hook (defines_tool) ───────────────────────────────────
  // The schema is parsed as YAML and re-serialized to JSON Schema string
  // so the JSONSchema validator can consume it without a YAML dep.
  if (auto dt = firmius::shared::FrontmatterParser::getMap(doc, "defines_tool")) {
    WorkflowDefinesTool spec;
    if (auto n = extractStringField(*dt, "name")) spec.name = *n;
    if (auto d = extractStringField(*dt, "description")) spec.description = *d;
    if (auto rs = extractStringField(*dt, "required_scope")) spec.requiredScope = *rs;
    if (auto ap = dt->find("applicable_personas"); ap != dt->end()) {
      if (const auto *arr =
              std::get_if<firmius::shared::FrontmatterValue::Array>(
                  &ap->second.value)) {
        for (const auto &elem : *arr) {
          if (const auto *s = std::get_if<std::string>(&elem.value)) {
            spec.applicablePersonas.push_back(*s);
          }
        }
      }
    }
    // Schema serialization: we walk the FrontmatterValue tree and emit
    // JSON. This is intentionally minimal — it matches the subset of YAML
    // used by JSON Schema (objects, arrays, strings, numbers, booleans).
    if (auto sc = dt->find("schema"); sc != dt->end()) {
      std::function<std::string(const firmius::shared::FrontmatterValue &)> toJson;
      toJson = [&](const firmius::shared::FrontmatterValue &v) -> std::string {
        if (const auto *s = std::get_if<std::string>(&v.value)) {
          // Escape minimal JSON characters.
          std::string out = "\"";
          for (char c : *s) {
            switch (c) {
              case '"':  out += "\\\""; break;
              case '\\': out += "\\\\"; break;
              case '\n': out += "\\n";  break;
              case '\r': out += "\\r";  break;
              case '\t': out += "\\t";  break;
              default:   out.push_back(c);
            }
          }
          out.push_back('"');
          return out;
        }
        if (const auto *b = std::get_if<bool>(&v.value)) {
          return *b ? "true" : "false";
        }
        if (const auto *i = std::get_if<int64_t>(&v.value)) {
          return std::to_string(*i);
        }
        if (const auto *arr =
                std::get_if<firmius::shared::FrontmatterValue::Array>(&v.value)) {
          std::string out = "[";
          for (std::size_t i = 0; i < arr->size(); ++i) {
            if (i > 0) out += ",";
            out += toJson((*arr)[i]);
          }
          out.push_back(']');
          return out;
        }
        if (const auto *m =
                std::get_if<firmius::shared::FrontmatterValue::Map>(&v.value)) {
          std::string out = "{";
          bool first = true;
          for (const auto &[k, sub] : *m) {
            if (!first) out += ",";
            first = false;
            out += "\"" + k + "\":" + toJson(sub);
          }
          out.push_back('}');
          return out;
        }
        return "null";
      };
      spec.schemaJson = toJson(sc->second);
    }
    if (!spec.name.empty()) {
      workflow.definesTool = std::move(spec);
    }
  }

  if (auto ret = firmius::shared::FrontmatterParser::getMap(doc, "returns")) {
    if (auto schema = extractStringField(*ret, "schema")) {
      workflow.returns.schemaName = *schema;
    }
    if (auto schemaJson = extractStringField(*ret, "schema_json")) {
      workflow.returns.schemaJson = *schemaJson;
    }
  }

  if (auto guard = firmius::shared::FrontmatterParser::getMap(doc, "guard")) {
    if (auto rm = extractStringField(*guard, "requires_mode")) {
      if (!rm->empty()) {
        workflow.guard.requiresMode = *rm;
      }
    }
    if (auto lm = extractBoolField(*guard, "lock_mode")) {
      workflow.guard.lockMode = *lm;
    }
    if (auto pre = guard->find("preconditions"); pre != guard->end()) {
      if (const auto *arr =
              std::get_if<firmius::shared::FrontmatterValue::Array>(
                  &pre->second.value)) {
        for (const auto &elem : *arr) {
          if (const auto *m =
                  std::get_if<firmius::shared::FrontmatterValue::Map>(
                      &elem.value)) {
            WorkflowPrecondition cond;
            if (auto k = extractStringField(*m, "kind")) {
              cond.kind = *k;
            }
            for (const auto &[key, val] : *m) {
              if (key == "kind") continue;
              if (const auto *s = std::get_if<std::string>(&val.value)) {
                cond.params[key] = *s;
              }
            }
            if (!cond.kind.empty()) {
              workflow.guard.preconditions.push_back(std::move(cond));
            }
          }
        }
      }
    }
  }
  // ─────────────────────────────────────────────────────────────────────────

  if (auto argsArray = firmius::shared::FrontmatterParser::getArray(doc, "args")) {
    for (const auto &item : *argsArray) {
      if (const auto *argMap = std::get_if<firmius::shared::FrontmatterValue::Map>(&item.value)) {
        WorkflowArg arg;
        arg.optional = false;
        
        auto it = argMap->find("name");
        if (it != argMap->end()) {
          if (const auto *s = std::get_if<std::string>(&it->second.value)) {
            arg.name = *s;
          }
        }
        
        it = argMap->find("type");
        if (it != argMap->end()) {
          if (const auto *s = std::get_if<std::string>(&it->second.value)) {
            std::string typeStr = *s;
            if (typeStr == "string") arg.type = WorkflowArgType::String;
            else if (typeStr == "number") arg.type = WorkflowArgType::Number;
            else if (typeStr == "filepath") arg.type = WorkflowArgType::Filepath;
            else if (typeStr == "agent_id") arg.type = WorkflowArgType::AgentId;
            else if (typeStr == "thread_id") arg.type = WorkflowArgType::ThreadId;
            else arg.type = WorkflowArgType::String;
          }
        }
        
        it = argMap->find("description");
        if (it != argMap->end()) {
          if (const auto *s = std::get_if<std::string>(&it->second.value)) {
            arg.description = *s;
          }
        }
        
        it = argMap->find("optional");
        if (it != argMap->end()) {
          if (const auto *b = std::get_if<bool>(&it->second.value)) {
            arg.optional = *b;
          } else if (const auto *s = std::get_if<std::string>(&it->second.value)) {
            arg.optional = (*s == "true" || *s == "yes" || *s == "1");
          } else if (const auto *i = std::get_if<int64_t>(&it->second.value)) {
            arg.optional = (*i != 0);
          }
        }
        if (!arg.name.empty()) {
          workflow.args.push_back(arg);
        }
      }
    }
  }

  // Set defaults if not provided
  if (workflow.name.empty()) {
    std::string name = workflow.id;
    std::replace(name.begin(), name.end(), '_', ' ');
    bool newWord = true;
    for (auto &c : name) {
      if (c == ' ') newWord = true;
      else if (newWord) { c = std::toupper(c); newWord = false; }
      else c = std::tolower(c);
    }
    workflow.name = name;
  }

  if (workflow.description.empty()) {
    workflow.description = "Execute workflow: " + workflow.name;
  }

  // Count argument placeholders ($1, $2, etc.) for legacy support
  std::regex argPattern(R"(\$([0-9]+))");
  auto begin = std::sregex_iterator(workflow.body.begin(), workflow.body.end(), argPattern);
  auto end = std::sregex_iterator();
  size_t maxArg = 0;
  for (auto it = begin; it != end; ++it) {
    std::smatch match = *it;
    maxArg = std::max(maxArg, (size_t)std::stoul(match[1].str()));
  }
  workflow.argCount = maxArg;

  if (workflow.args.empty() && workflow.argCount > 0) {
    for (size_t i = 0; i < workflow.argCount; ++i) {
      WorkflowArg arg;
      arg.name = "arg" + std::to_string(i + 1);
      arg.type = WorkflowArgType::String;
      arg.description = "Argument " + std::to_string(i + 1);
      arg.optional = false;
      workflow.args.push_back(arg);
    }
  }

  return workflow;
}

std::optional<Workflow> WorkflowLoader::loadYamlWorkflow(const std::string &path) {
  return loadWorkflow(path);
}

std::string WorkflowLoader::getWorkflowsDir() const {
  const char *envDir = std::getenv("FIRMIUS_WORKFLOWS_DIR");
  if (envDir) {
    const std::filesystem::path dir(envDir);
    if (isUsableWorkflowDir(dir)) {
      return ensureTrailingSlash(dir.string());
    }
  }

  const std::filesystem::path userDir = resolveWritableFirmiusHome() / "workflows";
  if (isUsableWorkflowDir(userDir)) {
    return ensureTrailingSlash(userDir.string());
  }

  return ensureTrailingSlash("workflows");
}

std::vector<std::string> WorkflowLoader::getHookDirs() const {
  std::vector<std::string> dirs;
  auto addIfUsable = [&dirs](const std::filesystem::path &dir) {
    if (std::filesystem::exists(dir) && std::filesystem::is_directory(dir)) {
      const auto normalized = ensureTrailingSlash(dir.string());
      if (std::find(dirs.begin(), dirs.end(), normalized) == dirs.end()) {
        dirs.push_back(normalized);
      }
    }
  };

  if (const char *envDir = std::getenv("FIRMIUS_HOOKS_DIR")) {
    const std::filesystem::path dir(envDir);
    addIfUsable(dir);
    if (!dirs.empty()) {
      return dirs;
    }
  }

  addIfUsable(std::filesystem::current_path() / "prompts" / "hooks");

  {
    const std::filesystem::path firmiusHome =
        firmius::shared::PlatformPaths::firmiusHomeDir();
    addIfUsable(firmiusHome / "prompts" / "hooks");
    addIfUsable(firmiusHome / "hooks");
  }

  const std::filesystem::path localHome =
      std::filesystem::current_path() / ".firmius";
  addIfUsable(localHome / "prompts" / "hooks");
  addIfUsable(localHome / "hooks");

  return dirs;
}

} // namespace firmius::core
