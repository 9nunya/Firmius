#include "agents/PurposeLoader.hpp"
#include "agents/prompts/PromptComposer.hpp"
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
  throw std::runtime_error("Unknown scope: " + s);
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

std::string PurposeLoader::composeSystemPrompt(const Persona &persona,
                                               const AgentContext &context,
                                               const std::string &toolsBlock) {
  // Layered composition. Stable layers (House Doctrine, Output Style,
  // Persona Voice, Profile, Hinting) come first so prompt-cache hits stay
  // warm; per-turn layers (Environment, Active Context) follow; scoped
  // frames (Mode, Workflow, Branch, Pact) render only when their owning
  // subsystem reports active state via PromptInputs. Adding a new section
  // is one registration call in PromptComposer::buildDefault().
  prompts::PromptInputs inputs;
  inputs.persona = &persona;
  inputs.context = &context;
  inputs.toolsBlock = toolsBlock;
  // Surface the live activeMode from AgentState so ModeOverlaySection +
  // ModeFrameSection render the correct sub-mode prompt every turn.
  if (!context.state.activeMode.empty()) {
    inputs.activeModeName = context.state.activeMode;
  }

  // Custom placeholders need to flow through the composer's HouseDoctrine
  // section. We re-apply them here on top of the composed output for
  // backward compatibility with anything that has registered keys outside
  // the standard set.
  std::string composed = prompts::PromptComposer::buildDefault().compose(inputs);
  if (!customPlaceholders.empty()) {
    for (const auto &[key, value] : customPlaceholders) {
      std::size_t pos = 0;
      while ((pos = composed.find(key, pos)) != std::string::npos) {
        composed.replace(pos, key.length(), value);
        pos += value.length();
      }
    }
  }
  return composed;
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
    std::filesystem::remove_all(userDir);
    std::filesystem::create_directories(userDir);
  } catch (const std::filesystem::filesystem_error &) {
    return;
  }
  // Mirror every prompt file, *including* nested subdirectories like
  // `modes/<persona>/*.md` for persona-scoped sub-modes. Without recursion,
  // any persona who carries an internal sub-mode set (forge, aster, fast,
  // glimmer, harbor, loom, meridian, vellum, witness, …) would have their
  // sub-modes invisible at runtime once the user cache exists.
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(builtinDir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::filesystem::path relative =
        std::filesystem::relative(entry.path(), builtinDir);
    std::filesystem::path target = userDir / relative;
    try {
      std::filesystem::create_directories(target.parent_path());
      std::filesystem::copy_file(
          entry.path(), target,
          std::filesystem::copy_options::overwrite_existing);
    } catch (const std::filesystem::filesystem_error &) {
      // Best-effort cache population only. Startup must continue using
      // readable built-in prompts when the user prompt cache is unwritable.
    }
  }
}

} // namespace firmius::core
