#include "agents/SkillLoader.hpp"
#include "utils/FrontmatterParser.hpp"
#include "utils/StringUtil.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string readFile(const std::filesystem::path &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return "";
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string ensureTrailingSlash(std::string dir) {
  if (!dir.empty() && dir.back() != '/') {
    dir.push_back('/');
  }
  return dir;
}

bool containsPath(const std::vector<std::string> &paths,
                  const std::string &candidate) {
  for (const auto &existing : paths) {
    if (existing == candidate) {
      return true;
    }
  }
  return false;
}

bool isValidSkillId(std::string_view skillId) {
  if (skillId.empty()) {
    return false;
  }
  if (skillId.find('/') != std::string_view::npos ||
      skillId.find('\\') != std::string_view::npos) {
    return false;
  }
  if (skillId == "." || skillId == "..") {
    return false;
  }
  return true;
}

} // namespace

namespace firmius::core {

std::vector<std::string> SkillLoader::resolveSkillsDirs() {
  std::vector<std::string> resolvedDirs;

  const char *skillsDirFromEnv = std::getenv("FIRMIUS_SKILLS_DIR");
  if (skillsDirFromEnv != nullptr && *skillsDirFromEnv != '\0') {
    const std::filesystem::path envPath =
        std::filesystem::path(skillsDirFromEnv).lexically_normal();
    if (std::filesystem::exists(envPath) && std::filesystem::is_directory(envPath)) {
      const std::string normalized = ensureTrailingSlash(envPath.string());
      if (!containsPath(resolvedDirs, normalized)) {
        resolvedDirs.push_back(normalized);
      }
    }
  }

  const char *home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    const auto homeSkills =
        (std::filesystem::path(home) / ".agents" / "skills").lexically_normal();
    if (std::filesystem::exists(homeSkills) &&
        std::filesystem::is_directory(homeSkills)) {
      const std::string normalized = ensureTrailingSlash(homeSkills.string());
      if (!containsPath(resolvedDirs, normalized)) {
        resolvedDirs.push_back(normalized);
      }
    }
  }

  const auto localSkills = std::filesystem::path(".agents") / "skills";
  if (std::filesystem::exists(localSkills) && std::filesystem::is_directory(localSkills)) {
    const std::string normalized =
        ensureTrailingSlash(localSkills.lexically_normal().string());
    if (!containsPath(resolvedDirs, normalized)) {
      resolvedDirs.push_back(normalized);
    }
  }

  return resolvedDirs;
}

std::optional<std::string>
SkillLoader::lookForSkillBaseDir(const std::string_view skillId) {
  if (!isValidSkillId(skillId)) {
    return std::nullopt;
  }

  const auto skillDirs = resolveSkillsDirs();
  for (const auto &baseDir : skillDirs) {
    const std::filesystem::path rootCandidate =
        (std::filesystem::path(baseDir) / std::string(skillId)).lexically_normal();
    if (std::filesystem::exists(rootCandidate) &&
        std::filesystem::is_directory(rootCandidate)) {
      return rootCandidate.string();
    }
  }

  return std::nullopt;
}

std::optional<std::string>
SkillLoader::resolveSkillRoot(const std::string_view skillId) {
  return lookForSkillBaseDir(skillId);
}

std::optional<shared::AgentSkill>
SkillLoader::getSkill(const std::string_view skillId) {
  auto skillBaseDir = lookForSkillBaseDir(skillId);
  if (!skillBaseDir.has_value()) {
    return std::nullopt;
  }

  const std::filesystem::path skillRoot(skillBaseDir.value());
  const std::filesystem::path skillMdPath =
      (skillRoot / "SKILL.md").lexically_normal();
  if (!std::filesystem::exists(skillMdPath) ||
      !std::filesystem::is_regular_file(skillMdPath)) {
    return std::nullopt;
  }

  const std::string markdown = readFile(skillMdPath);
  if (markdown.empty()) {
    return std::nullopt;
  }

  shared::AgentSkill skill;
  skill.skillId = std::string(skillId);
  skill.skillRoot = skillRoot.string();
  skill.skillWd = skill.skillRoot;
  skill.skillMdPath = skillMdPath.string();

  const auto doc =
      shared::FrontmatterParser::parseMarkdown(markdown, skill.skillMdPath);
  skill.name = shared::FrontmatterParser::getString(doc, "name")
                   .value_or(std::string(skillId));
  skill.description =
      shared::FrontmatterParser::getString(doc, "description").value_or("");
  skill.entrypointBody = shared::StringUtil::trim(doc.body);

  return skill;
}

} // namespace firmius::core
