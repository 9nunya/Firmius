#include "agents/PurposeLoader.hpp"
#include "ConfigLoader.hpp"
#include "agents/HintingLoader.hpp"
#include "utils/FrontmatterParser.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @file PurposeLoader.cpp
 * @brief Implementation of agent persona loading and prompt composition.
 */

namespace {
const std::array<const char *, 7> kLegacyPromptFiles = {
    "brainstormer.md", "builder.md", "coordinator.md", "firmius.md",
    "general.md",      "planner.md", "reviewer.md"};

PurposeWorkRole legacyWorkRoleForPurposeName(const std::string &purpose) {
  const std::string lowered = StringUtil::toLower(StringUtil::trim(purpose));
  if (lowered == "aster" || lowered == "meridian" || lowered == "vellum" ||
      lowered == "harbor" || lowered == "fast") {
    return PurposeWorkRole::Lead;
  }
  if (lowered == "forge") {
    return PurposeWorkRole::Executor;
  }
  if (lowered == "ember") {
    return PurposeWorkRole::Worker;
  }
  if (lowered == "witness" || lowered == "loom") {
    return PurposeWorkRole::Auditor;
  }
  if (lowered == "glimmer") {
    return PurposeWorkRole::Scout;
  }
  return PurposeWorkRole::Unknown;
}

std::string ensureTrailingSlash(std::string dir) {
  if (!dir.empty() && dir.back() != '/') {
    dir += '/';
  }
  return dir;
}

bool isReadableFile(const std::filesystem::path &path) {
  std::ifstream file(path);
  return file.good();
}

bool isUsablePromptsDir(const std::filesystem::path &dir) {
  try {
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
      return false;
    }

    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".md") {
        continue;
      }
      if (isReadableFile(entry.path())) {
        return true;
      }
    }
    return false;
  } catch (...) {
    return false;
  }
}
/**
 * @brief Maps a string scope identifier to the ToolScope enum.
 */
firmius::shared::ToolScope stringToScope(const std::string &s) {
  using firmius::shared::ToolScope;
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
  if (s == "plan:read" || s == "PlanRead")
    return ToolScope::PlanRead;
  if (s == "plan:write" || s == "PlanWrite")
    return ToolScope::PlanWrite;
  if (s == "chunk:read" || s == "ChunkRead")
    return ToolScope::ChunkRead;
  if (s == "chunk:write" || s == "ChunkWrite")
    return ToolScope::ChunkWrite;
  if (s == "chunk:assign" || s == "ChunkAssign")
    return ToolScope::ChunkAssign;
  if (s == "chunk:review" || s == "ChunkReview")
    return ToolScope::ChunkReview;
  throw std::runtime_error("Unknown scope: " + s);
}

PurposeWorkRole stringToPurposeWorkRole(const std::string &value,
                                        const std::string &fieldName) {
  const std::string lowered = StringUtil::toLower(StringUtil::trim(value));
  if (lowered == "lead" || lowered == "aster" || lowered == "meridian" ||
      lowered == "vellum" || lowered == "harbor" || lowered == "fast") {
    return PurposeWorkRole::Lead;
  }
  if (lowered == "executor" || lowered == "forge") {
    return PurposeWorkRole::Executor;
  }
  if (lowered == "worker" || lowered == "ember") {
    return PurposeWorkRole::Worker;
  }
  if (lowered == "auditor" || lowered == "witness" || lowered == "loom") {
    return PurposeWorkRole::Auditor;
  }
  if (lowered == "scout" || lowered == "glimmer") {
    return PurposeWorkRole::Scout;
  }
  throw std::runtime_error(
      fieldName + " must be one of: aster, meridian, vellum, glimmer, forge, ember, witness, harbor, loom, fast");
}

std::optional<std::filesystem::path>
canonicalProjectRoot(const AgentContext &context) {
  if (StringUtil::trim(context.environment.cwd).empty()) {
    return std::nullopt;
  }

  try {
    const std::filesystem::path cwdPath(context.environment.cwd);
    return std::filesystem::weakly_canonical(cwdPath);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::filesystem::path>
canonicalAgentsFileForDir(const std::filesystem::path &dir) {
  try {
    const std::filesystem::path candidate = dir / "AGENTS.md";
    if (!std::filesystem::exists(candidate) ||
        !std::filesystem::is_regular_file(candidate)) {
      return std::nullopt;
    }
    return std::filesystem::weakly_canonical(candidate);
  } catch (...) {
    return std::nullopt;
  }
}

bool hasLoadedAgentsPath(const AgentContext &context,
                         const std::string &canonicalPath) {
  return std::find(context.state.loadedAgentMds.begin(),
                   context.state.loadedAgentMds.end(),
                   canonicalPath) != context.state.loadedAgentMds.end();
}

void recordLoadedAgentsPath(AgentContext &context,
                            const std::string &canonicalAgentsPath,
                            const std::string &projectRoot) {
  if (!hasLoadedAgentsPath(context, canonicalAgentsPath)) {
    context.state.loadedAgentMds.push_back(canonicalAgentsPath);
  }
  context.state.loadedSkillRoots[canonicalAgentsPath] = projectRoot;
}
} // namespace

std::map<std::string, std::string> PurposeLoader::customPlaceholders;

std::string purposeWorkRoleToString(PurposeWorkRole role) {
  switch (role) {
  case PurposeWorkRole::Lead:
    return "aster";
  case PurposeWorkRole::Executor:
    return "forge";
  case PurposeWorkRole::Worker:
    return "ember";
  case PurposeWorkRole::Auditor:
    return "witness";
  case PurposeWorkRole::Scout:
    return "glimmer";
  case PurposeWorkRole::Unknown:
    return "unknown";
  }
  return "unknown";
}

PurposeWorkRole purposeWorkRoleFromString(const std::string &role) {
  return stringToPurposeWorkRole(role, "work_role");
}

void PurposeLoader::registerPlaceholder(const std::string &key,
                                        const std::string &value) {
  customPlaceholders[key] = value;
}

bool PurposeLoader::isValid(const std::string &purpose) {
  if (purpose.empty())
    return false;
  std::string promptsDir = resolvePromptsDir();
  try {
    std::string path = promptsDir + purpose + ".md";
    return std::filesystem::exists(path);
  } catch (...) {
    return false;
  }
}

Persona PurposeLoader::load(const std::string &purpose) {
  std::string promptsDir = resolvePromptsDir();
  std::string path = promptsDir + purpose + ".md";
  std::ifstream file(path);
  if (!file.is_open()) {
    std::vector<std::string> purposes;
    if (std::filesystem::exists(promptsDir)) {
      for (const auto &entry :
           std::filesystem::directory_iterator(promptsDir)) {
        if (entry.path().extension() == ".md" &&
            entry.path().stem() != "base" &&
            entry.path().stem() != "COMPACTION_PROMPT") {
          purposes.push_back(entry.path().stem().string());
        }
      }
    }
    std::string purposeList;
    for (size_t i = 0; i < purposes.size(); ++i) {
      purposeList += "'" + purposes[i] + "'";
      if (i < purposes.size() - 1)
        purposeList += ", ";
    }
    throw std::runtime_error("Could not load persona '" + purpose +
                             "'. Available purposes are: " + purposeList);
  }

  Persona persona;
  persona.name = purpose;
  persona.purposeKey = purpose;
  std::stringstream buffer;
  buffer << file.rdbuf();
  const auto document = FrontmatterParser::parseMarkdown(buffer.str(), path);
  persona.identityPrompt = StringUtil::trim(document.body);

  if (auto name = FrontmatterParser::getString(document, "name")) {
    persona.name = *name;
  }
  if (auto title = FrontmatterParser::getString(document, "title")) {
    persona.title = *title;
  }
  if (auto description =
          FrontmatterParser::getString(document, "description")) {
    persona.description = *description;
  }
  if (auto canSpawn = FrontmatterParser::getBool(document, "canSpawn")) {
    persona.canSpawn = *canSpawn;
  }
  if (auto workRole = FrontmatterParser::getString(document, "work_role")) {
    persona.workRole = stringToPurposeWorkRole(*workRole, "work_role");
    persona.hasWorkRole = true;
  } else if (auto workRoleAlias =
                 FrontmatterParser::getString(document, "workRole")) {
    persona.workRole = stringToPurposeWorkRole(*workRoleAlias, "workRole");
    persona.hasWorkRole = true;
  }
  for (const auto &scopeName :
       FrontmatterParser::getStringArray(document, "scopes")) {
    persona.allowedScopes.push_back(stringToScope(scopeName));
  }
  if (auto switchable = FrontmatterParser::getBool(document, "switchable")) {
    persona.switchable = *switchable;
  }

  if (persona.title.empty()) {
    persona.title = persona.name;
  }

  return persona;
}

PurposeWorkRole PurposeLoader::resolveWorkRole(const Persona &persona) {
  if (persona.hasWorkRole) {
    return persona.workRole;
  }
  return legacyWorkRoleForPurposeName(persona.purposeKey);
}

PurposeWorkRole PurposeLoader::resolveWorkRole(const std::string &purpose) {
  if (!isValid(purpose)) {
    return legacyWorkRoleForPurposeName(purpose);
  }
  return resolveWorkRole(load(purpose));
}

std::string PurposeLoader::composeSystemPrompt(const Persona &persona,
                                               const AgentContext &context,
                                               const std::string &toolsBlock) {
  const std::string detectedModelFamily = ModelHintResolver::detectFamily(
      context.config.providerId, context.config.modelId,
      context.config.modelVariant);
  std::optional<HintingOverlay> hintingOverlay;
  try {
    hintingOverlay = HintingLoader::loadForModel(context.config.providerId,
                                                 context.config.modelId,
                                                 context.config.modelVariant);
  } catch (const std::exception &e) {
    std::cerr << "[hinting] Failed to load model hinting overlay: " << e.what()
              << "\n";
  } catch (...) {
    std::cerr << "[hinting] Failed to load model hinting overlay.\n";
  }

  std::string basePrompt;
  std::ifstream baseFile(resolvePromptsDir() + "base.md");
  if (baseFile.is_open()) {
    std::stringstream buffer;
    buffer << baseFile.rdbuf();
    basePrompt = buffer.str();
  }

  // Dynamic placeholders
  std::map<std::string, std::string> placeholders;
  placeholders["{{AGENT_NAME}}"] = persona.name;
  placeholders["{{AGENT_TITLE}}"] = persona.title;
  placeholders["{{CWD}}"] = context.environment.cwd;
  placeholders["{{MODEL_FAMILY}}"] = detectedModelFamily;
  placeholders["{{ACTIVE_HINTING_FILE}}"] =
      hintingOverlay.has_value() ? hintingOverlay->name : "none";

  std::string promptsDir = resolvePromptsDir();
  std::vector<std::string> purposes;
  if (std::filesystem::exists(promptsDir)) {
    for (const auto &entry : std::filesystem::directory_iterator(promptsDir)) {
      if (entry.path().extension() == ".md" && entry.path().stem() != "base" &&
          entry.path().stem() != "COMPACTION_PROMPT") {
        purposes.push_back(entry.path().stem().string());
      }
    }
  }
  std::string purposeList;
  std::sort(purposes.begin(), purposes.end());
  purposes.erase(std::unique(purposes.begin(), purposes.end()), purposes.end());
  for (size_t i = 0; i < purposes.size(); ++i) {
    purposeList += purposes[i];
    if (i < purposes.size() - 1)
      purposeList += ", ";
  }
  placeholders["{{REGISTERED_PURPOSES}}"] = purposeList;

  // Custom placeholders
  for (const auto &[key, value] : customPlaceholders) {
    placeholders[key] = value;
  }

  for (const auto &[key, value] : placeholders) {
    size_t pos = 0;
    while ((pos = basePrompt.find(key, pos)) != std::string::npos) {
      basePrompt.replace(pos, key.length(), value);
      pos += value.length();
    }
  }

  std::stringstream ss;
  ss << basePrompt << "\n\n";
  ss << "# AGENT IDENTITY\n" << persona.identityPrompt << "\n\n";

  if (hintingOverlay.has_value()) {
    ss << "# MODEL-SPECIFIC HINTING\n";
    ss << "Detected Family: " << detectedModelFamily << "\n";
    ss << "Hinting File: " << hintingOverlay->name << "\n\n";
    ss << hintingOverlay->body << "\n\n";
  }

  ss << "# ENVIRONMENT\n";
  ss << "Host: " << context.environment.identifier << "\n";
  ss << "CWD: " << context.environment.cwd << "\n\n";
  ss << "MODEL: " << context.config.modelId << "\n\n";

  ss << "Purposes Registered (you use these in summon_subagent): "
     << purposeList << "\n\n";

  const auto &cfg = shared::ConfigLoader::instance().getConfig();
  std::vector<std::string> category_names;
  category_names.reserve(cfg.modelRouterCategories.size());
  for (const auto &[name, _] : cfg.modelRouterCategories) {
    category_names.push_back(name);
  }
  std::sort(category_names.begin(), category_names.end());
  std::string categories_summary;
  for (size_t i = 0; i < category_names.size(); ++i) {
    if (i > 0) {
      categories_summary += ", ";
    }
    categories_summary += category_names[i];
  }
  if (categories_summary.empty()) {
    categories_summary = "(none)";
  }
  ss << "Model Route Categories: " << categories_summary << "\n";
  if (!cfg.defaultRouteCategory.empty()) {
    ss << "Default Route Category: " << cfg.defaultRouteCategory << "\n";
  }
  ss << "\n";

  if (!toolsBlock.empty()) {
    // literally not even needed
    // ss << "# AVAILABLE TOOLS\n" << toolsBlock << "\n";
  }

  return ss.str();
}

// deprecated
std::string PurposeLoader::buildToolsBlock(
    const std::vector<firmius::provider::ToolDefinition> &tools) {
  std::stringstream ss;
  for (const auto &t : tools) {
    ss << "- " << t.name << ": " << t.description << "\n";
    ss << "  Args: " << t.inputSchema << "\n";
  }
  return ss.str();
}

std::string PurposeLoader::loadCompactionPrompt() {
  std::ifstream file(resolvePromptsDir() + "COMPACTION_PROMPT.md");
  if (!file.is_open())
    return "You must summarize the session. Preserve critical state.";
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::optional<std::string>
PurposeLoader::resolveProjectRootAgentsPath(const AgentContext &context) {
  const auto projectRoot = canonicalProjectRoot(context);
  if (!projectRoot.has_value()) {
    return std::nullopt;
  }

  const auto rootAgents = canonicalAgentsFileForDir(*projectRoot);
  if (!rootAgents.has_value()) {
    return std::nullopt;
  }

  return rootAgents->string();
}

bool PurposeLoader::loadProjectRootAgentsIntoContext(AgentContext &context) {
  const auto projectRoot = canonicalProjectRoot(context);
  if (!projectRoot.has_value()) {
    return false;
  }

  const auto rootAgents = canonicalAgentsFileForDir(*projectRoot);
  if (!rootAgents.has_value()) {
    return false;
  }

  const std::string rootAgentsPath = rootAgents->string();
  if (hasLoadedAgentsPath(context, rootAgentsPath)) {
    context.state.loadedSkillRoots[rootAgentsPath] = projectRoot->string();
    return false;
  }

  recordLoadedAgentsPath(context, rootAgentsPath, projectRoot->string());
  return true;
}

std::vector<std::string>
PurposeLoader::discoverAncestorAgentsPaths(const std::string &targetPath,
                                           const AgentContext &context) {
  std::vector<std::string> discovered;
  const auto projectRoot = canonicalProjectRoot(context);
  if (!projectRoot.has_value()) {
    return discovered;
  }

  std::filesystem::path current;
  try {
    current =
        std::filesystem::weakly_canonical(std::filesystem::path(targetPath));
  } catch (...) {
    return discovered;
  }

  if (!std::filesystem::is_directory(current)) {
    current = current.parent_path();
  }

  if (current.empty()) {
    return discovered;
  }

  while (!current.empty() && current != *projectRoot &&
         FSUtil::isCanonicalSubpath(current, *projectRoot)) {
    const auto candidate = canonicalAgentsFileForDir(current);
    if (candidate.has_value()) {
      const std::string candidatePath = candidate->string();
      if (std::find(discovered.begin(), discovered.end(), candidatePath) ==
          discovered.end()) {
        discovered.push_back(candidatePath);
      }
    }

    const auto parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }

  return discovered;
}

std::size_t
PurposeLoader::loadDiscoveredAgentsForPath(AgentContext &context,
                                           const std::string &targetPath) {
  const auto projectRoot = canonicalProjectRoot(context);
  if (!projectRoot.has_value()) {
    return 0;
  }

  const std::optional<std::string> rootAgentsPath =
      resolveProjectRootAgentsPath(context);
  const std::vector<std::string> discovered =
      discoverAncestorAgentsPaths(targetPath, context);

  std::size_t addedCount = 0;
  for (const auto &path : discovered) {
    if (rootAgentsPath.has_value() && path == *rootAgentsPath) {
      continue;
    }
    if (hasLoadedAgentsPath(context, path)) {
      continue;
    }
    recordLoadedAgentsPath(context, path, projectRoot->string());
    addedCount++;
  }

  return addedCount;
}

std::string PurposeLoader::resolvePromptsDir() {
  const char *envDir = std::getenv("FIRMIUS_PROMPTS_DIR");
  if (envDir) {
    const std::filesystem::path dir(envDir);
    if (isUsablePromptsDir(dir)) {
      return ensureTrailingSlash(dir.string());
    }
  }

  const char *home = std::getenv("HOME");
  if (home) {
    const std::filesystem::path userDir =
        std::filesystem::path(home) / ".firmius" / "prompts";
    if (isUsablePromptsDir(userDir)) {
      return ensureTrailingSlash(userDir.string());
    }
  }

  return ensureTrailingSlash("prompts");
}

std::vector<std::string> PurposeLoader::listSwitchablePurposes() {
  std::vector<std::string> purposes = listPurposes();
  std::vector<std::string> switchable;
  for (const auto &purpose : purposes) {
    try {
      auto persona = load(purpose);
      if (persona.switchable) {
        switchable.push_back(purpose);
      }
    } catch (...) {
    }
  }
  return switchable;
}

std::vector<std::string> PurposeLoader::listPurposes() {
  std::vector<std::string> purposes;
  std::string promptsDir = resolvePromptsDir();
  if (!std::filesystem::exists(promptsDir)) {
    return purposes;
  }

  for (const auto &entry : std::filesystem::directory_iterator(promptsDir)) {
    if (entry.path().extension() != ".md")
      continue;
    auto stem = entry.path().stem().string();
    if (stem == "base" || stem == "COMPACTION_PROMPT")
      continue;
    purposes.push_back(stem);
  }

  std::sort(purposes.begin(), purposes.end());
  purposes.erase(std::unique(purposes.begin(), purposes.end()), purposes.end());
  return purposes;
}

void PurposeLoader::bootstrapDefaults(const std::string &builtinPromptsDir) {
  const char *home = std::getenv("HOME");
  if (!home)
    return;

  const std::filesystem::path builtinDir(builtinPromptsDir);
  if (!std::filesystem::exists(builtinDir))
    return;

  const std::filesystem::path userDir =
      std::filesystem::path(home) / ".firmius" / "prompts";

  try {
    std::filesystem::create_directories(userDir);
  } catch (const std::filesystem::filesystem_error &) {
    return;
  }

  for (const auto *legacyFile : kLegacyPromptFiles) {
    try {
      std::filesystem::remove(userDir / legacyFile);
    } catch (const std::filesystem::filesystem_error &) {
    }
  }
  for (const auto &entry : std::filesystem::directory_iterator(builtinDir)) {
    if (entry.is_regular_file()) {
      std::filesystem::path target = userDir / entry.path().filename();
      try {
        std::filesystem::copy_file(
            entry.path(), target,
            std::filesystem::copy_options::overwrite_existing);
      } catch (const std::filesystem::filesystem_error &) {
        // Best-effort cache population only. Startup must continue using
        // readable built-in prompts when the user prompt cache is unwritable.
      }
    }
  }
}

} // namespace firmius::core
