#include "agents/modes/Mode.hpp"

#include "agents/PurposeLoader.hpp"
#include "utils/FrontmatterParser.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace firmius::core::modes {

using firmius::shared::FrontmatterParser;
using firmius::shared::FrontmatterDocument;
using firmius::shared::StringUtil;
using firmius::shared::ToolScope;

namespace {

ToolScope scopeFromString(const std::string &s) {
  if (s == "fs:read" || s == "FilesystemRead")
    return ToolScope::FilesystemRead;
  if (s == "fs:write" || s == "FilesystemWrite")
    return ToolScope::FilesystemWrite;
  if (s == "process:exec" || s == "Process")
    return ToolScope::Process;
  if (s == "semantic" || s == "Semantic")
    return ToolScope::Semantic;
  if (s == "delegation" || s == "Delegation")
    return ToolScope::Delegation;
  if (s == "web" || s == "Web")
    return ToolScope::Web;
  if (s == "git" || s == "Git")
    return ToolScope::Git;
  if (s == "crew:read" || s == "CrewRead" || s == "plan:read" || s == "PlanRead" ||
      s == "chunk:read" || s == "ChunkRead")
    return ToolScope::CrewRead;
  if (s == "crew:write" || s == "CrewWrite" || s == "plan:write" || s == "PlanWrite" ||
      s == "chunk:write" || s == "ChunkWrite")
    return ToolScope::CrewWrite;
  if (s == "crew:assign" || s == "CrewAssign" || s == "chunk:assign" || s == "ChunkAssign")
    return ToolScope::CrewAssign;
  if (s == "crew:review" || s == "CrewReview" || s == "chunk:review" || s == "ChunkReview")
    return ToolScope::CrewReview;
  throw std::runtime_error("Unknown tool scope in mode file: " + s);
}

std::string readFileContents(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open mode file: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

const Mode *findPersonaAlias(const ModeRegistry &registry,
                             const std::string &persona,
                             const std::string &requested) {
  if (persona.empty()) {
    return nullptr;
  }

  const std::string normalized = StringUtil::toLower(StringUtil::trim(requested));
  if (normalized.empty()) {
    return nullptr;
  }

  static const std::map<std::string, std::vector<std::string>> kAliasTargets = {
      {"explore", {"route", "scan", "recon"}},
      {"analysis", {"route", "scan", "recon"}},
      {"analyze", {"route", "scan", "recon"}},
      {"investigate", {"route", "scan", "recon"}},
      {"recon", {"recon", "scan", "route"}},
  };

  auto it = kAliasTargets.find(normalized);
  if (it == kAliasTargets.end()) {
    return nullptr;
  }

  for (const auto &candidate : it->second) {
    if (const Mode *mode = registry.find(persona + ":" + candidate)) {
      return mode;
    }
  }
  return nullptr;
}

} // namespace

std::string resolveModesDir() {
  // PurposeLoader::resolvePromptsDir already encodes the env-var → user dir
  // → builtin search order; we just append /modes/.
  std::string base = PurposeLoader::resolvePromptsDir();
  if (base.empty()) {
    return base;
  }
  if (base.back() != '/') {
    base += '/';
  }
  base += "modes/";
  return base;
}

Mode loadModeFromFile(const std::string &path) {
  Mode mode;
  mode.sourcePath = path;

  const std::string raw = readFileContents(path);
  const FrontmatterDocument doc = FrontmatterParser::parseMarkdown(raw, path);

  if (auto v = FrontmatterParser::getString(doc, "name")) {
    mode.name = *v;
  }
  if (auto v = FrontmatterParser::getString(doc, "title")) {
    mode.title = *v;
  }
  if (auto v = FrontmatterParser::getString(doc, "glyph")) {
    mode.glyph = *v;
  }
  if (auto v = FrontmatterParser::getString(doc, "short")) {
    mode.shortDescription = *v;
  }
  if (auto v = FrontmatterParser::getString(doc, "output_schema")) {
    mode.outputSchema = *v;
  }
  if (auto v = FrontmatterParser::getString(doc, "parent_mode")) {
    if (!v->empty()) {
      mode.parentMode = *v;
    }
  }
  if (auto v = FrontmatterParser::getString(doc, "persona_scope")) {
    if (!v->empty()) {
      mode.personaScope = *v;
    }
  }
  for (const auto &p :
       FrontmatterParser::getStringArray(doc, "applicable_personas")) {
    mode.applicablePersonas.push_back(p);
  }
  for (const auto &p :
       FrontmatterParser::getStringArray(doc, "auto_workflows_on_enter")) {
    mode.autoWorkflowsOnEnter.push_back(p);
  }
  for (const auto &p :
       FrontmatterParser::getStringArray(doc, "allowed_transitions_to")) {
    mode.allowedTransitionsTo.push_back(p);
  }

  // tool_scope is a map: { allow: [...], deny: [...] }
  if (auto scopeMap = FrontmatterParser::getMap(doc, "tool_scope")) {
    auto pullScopes = [&](const std::string &key,
                          std::vector<ToolScope> &dest) {
      auto it = scopeMap->find(key);
      if (it == scopeMap->end()) {
        return;
      }
      const auto *arr =
          std::get_if<firmius::shared::FrontmatterValue::Array>(
              &it->second.value);
      if (!arr) {
        return;
      }
      for (const auto &elem : *arr) {
        if (const auto *s = std::get_if<std::string>(&elem.value)) {
          dest.push_back(scopeFromString(*s));
        }
      }
    };
    pullScopes("allow", mode.allowScopes);
    pullScopes("deny", mode.denyScopes);
  }

  mode.promptOverlay = StringUtil::trim(doc.body);

  if (mode.name.empty()) {
    throw std::runtime_error("Mode file " + path + " is missing 'name'");
  }
  if (mode.title.empty()) {
    mode.title = mode.name;
  }
  return mode;
}

ModeRegistry &ModeRegistry::instance() {
  static ModeRegistry singleton;
  static bool firstLoad = false;
  if (!firstLoad) {
    firstLoad = true;
    singleton.reload();
  }
  return singleton;
}

void ModeRegistry::reload() {
  modes_.clear();
  const std::string dir = resolveModesDir();
  if (dir.empty() || !std::filesystem::exists(dir)) {
    return;
  }

  // Pass 1: system-level modes at prompts/modes/*.md.
  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".md") {
      try {
        Mode m = loadModeFromFile(entry.path().string());
        modes_[m.qualifiedName()] = std::move(m);
      } catch (const std::exception &e) {
        std::cerr << "[modes] Skipping " << entry.path() << ": " << e.what()
                  << "\n";
      }
    }
  }

  // Pass 2: persona-scoped sub-modes at prompts/modes/<persona>/*.md.
  // The directory name becomes the personaScope unless the file overrides
  // it explicitly via frontmatter. This lets users drop a quick
  // ~/.firmius/modes/<persona>/<submode>.md file with no boilerplate.
  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_directory()) {
      continue;
    }
    const std::string personaScope = entry.path().filename().string();
    for (const auto &subEntry :
         std::filesystem::directory_iterator(entry.path())) {
      if (!subEntry.is_regular_file() || subEntry.path().extension() != ".md") {
        continue;
      }
      try {
        Mode m = loadModeFromFile(subEntry.path().string());
        if (!m.personaScope.has_value() || m.personaScope->empty()) {
          m.personaScope = personaScope;
        }
        modes_[m.qualifiedName()] = std::move(m);
      } catch (const std::exception &e) {
        std::cerr << "[modes] Skipping " << subEntry.path() << ": " << e.what()
                  << "\n";
      }
    }
  }
}

const Mode *ModeRegistry::find(const std::string &qualifiedName) const {
  auto it = modes_.find(qualifiedName);
  if (it == modes_.end()) {
    return nullptr;
  }
  return &it->second;
}

const Mode *ModeRegistry::resolveForPersona(const std::string &name,
                                            const std::string &persona) const {
  // Already qualified ("forge:apply") — look up verbatim.
  if (name.find(':') != std::string::npos) {
    return find(name);
  }
  // Bare name with active persona — try persona-scoped first, fall back
  // to system mode. This is what mode_switch("apply") does for an active
  // Forge agent: prefer Forge's apply, fall back to a hypothetical system
  // "apply" mode if it ever exists.
  if (!persona.empty()) {
    if (const Mode *m = find(persona + ":" + name)) {
      return m;
    }
  }
  if (const Mode *m = find(name)) {
    return m;
  }
  return findPersonaAlias(*this, persona, name);
}

std::vector<std::string> ModeRegistry::listNames() const {
  std::vector<std::string> names;
  names.reserve(modes_.size());
  for (const auto &[name, _] : modes_) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string>
ModeRegistry::listForPersona(const std::string &persona) const {
  std::vector<std::string> names;
  for (const auto &[qname, mode] : modes_) {
    if (mode.personaScope.has_value() && *mode.personaScope == persona) {
      names.push_back(qname);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

} // namespace firmius::core::modes
