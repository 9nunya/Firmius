#include "agents/HintingLoader.hpp"
#include "utils/FrontmatterParser.hpp"
#include "utils/StringUtil.hpp"
#include "utils/PlatformPaths.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace firmius::core {

using namespace firmius::shared;

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

bool isUsableHintingDir(const std::filesystem::path &dir) {
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

std::string normalizeModelToken(std::string value) {
  value = StringUtil::toLower(StringUtil::trim(value));
  if (value.empty()) {
    return value;
  }

  // Strip provider prefix "provider/model".
  const size_t slash = value.rfind('/');
  if (slash != std::string::npos) {
    value = value.substr(slash + 1);
  }

  // Strip obvious transport prefixes.
  const std::vector<std::string> prefixes = {"models/", "model/", "openai/"};
  for (const auto &prefix : prefixes) {
    if (value.rfind(prefix, 0) == 0) {
      value = value.substr(prefix.size());
      break;
    }
  }

  return value;
}

bool startsWithToken(const std::string &value, const std::string &token) {
  if (value == token) {
    return true;
  }
  if (value.rfind(token + "-", 0) == 0) {
    return true;
  }
  if (value.rfind(token + "_", 0) == 0) {
    return true;
  }
  if (value.rfind(token + ":", 0) == 0) {
    return true;
  }
  return false;
}

bool isGptFamilyName(const std::string &value) {
  if (startsWithToken(value, "gpt")) {
    return true;
  }
  // OpenAI reasoning line aliases.
  return startsWithToken(value, "o1") || startsWithToken(value, "o3") ||
         startsWithToken(value, "o4");
}

} // namespace

std::string ModelHintResolver::detectFamily(const std::string &providerId,
                                            const std::string &modelId,
                                            const std::string &variantName) {
  const std::string normalizedModel = normalizeModelToken(modelId);
  const std::string normalizedVariant = normalizeModelToken(variantName);
  const std::string normalizedProvider = normalizeModelToken(providerId);

  const std::vector<std::string> candidates = {
      normalizedModel, normalizedVariant, normalizedProvider};

  for (const auto &candidate : candidates) {
    if (candidate.empty()) {
      continue;
    }
    if (isGptFamilyName(candidate)) {
      return "gpt";
    }
    if (startsWithToken(candidate, "claude")) {
      return "claude";
    }
    if (startsWithToken(candidate, "gemini")) {
      return "gemini";
    }
    if (startsWithToken(candidate, "qwen") || startsWithToken(candidate, "qwq")) {
      return "qwen";
    }
    if (startsWithToken(candidate, "deepseek")) {
      return "deepseek";
    }
  }

  return "generic";
}

std::vector<std::string> HintingLoader::resolveHintingDirs() {
  std::vector<std::string> dirs;

  const char *envDir = std::getenv("FIRMIUS_HINTING_DIR");
  if (envDir) {
    const std::filesystem::path dir(envDir);
    if (isUsableHintingDir(dir)) {
      dirs.push_back(ensureTrailingSlash(dir.string()));
    }
  }

  const std::filesystem::path userDir = PlatformPaths::firmiusHomeDir() / "hinting";
  if (isUsableHintingDir(userDir)) {
    dirs.push_back(ensureTrailingSlash(userDir.string()));
  }

  const std::filesystem::path builtinDir("hinting");
  if (isUsableHintingDir(builtinDir)) {
    dirs.push_back(ensureTrailingSlash(builtinDir.string()));
  }

  return dirs;
}

std::string HintingLoader::resolveHintingDir() {
  const auto dirs = resolveHintingDirs();
  if (!dirs.empty()) {
    return dirs.front();
  }
  return ensureTrailingSlash("hinting");
}

std::optional<HintingOverlay>
HintingLoader::loadFromPath(const std::string &family, const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  FrontmatterDocument doc;
  try {
    doc = FrontmatterParser::parseMarkdown(content, path);
  } catch (const std::exception &e) {
    std::cerr << "[hinting] Failed to parse frontmatter in '" << path << "': " << e.what() << "\n";
    return std::nullopt;
  }

  HintingOverlay overlay;
  overlay.name = FrontmatterParser::getString(doc, "name").value_or(family);
  overlay.title = FrontmatterParser::getString(doc, "title").value_or(family);
  overlay.description = FrontmatterParser::getString(doc, "description").value_or("");
  overlay.builtin = FrontmatterParser::getBool(doc, "builtin").value_or(false);
  overlay.enabled = FrontmatterParser::getBool(doc, "enabled").value_or(true);
  overlay.priority = static_cast<int>(FrontmatterParser::getInt(doc, "priority").value_or(0));
  overlay.body = StringUtil::trim(doc.body);
  overlay.sourcePath = path;

  if (overlay.body.empty()) {
    std::cerr << "[hinting] Hinting file '" << path
              << "' has empty body and will be ignored.\n";
    return std::nullopt;
  }
  if (!overlay.enabled) {
    return std::nullopt;
  }

  return overlay;
}

std::optional<HintingOverlay>
HintingLoader::loadByFamily(const std::string &family) {
  const std::string normalizedFamily =
      StringUtil::toLower(StringUtil::trim(family));
  if (normalizedFamily.empty()) {
    return std::nullopt;
  }

  const auto dirs = resolveHintingDirs();
  for (const auto &dir : dirs) {
    const std::string path = dir + normalizedFamily + ".md";
    auto overlay = loadFromPath(normalizedFamily, path);
    if (overlay.has_value()) {
      return overlay;
    }
  }
  return std::nullopt;
}

std::optional<HintingOverlay>
HintingLoader::loadForModel(const std::string &providerId,
                            const std::string &modelId,
                            const std::string &variantName) {
  const std::string family =
      ModelHintResolver::detectFamily(providerId, modelId, variantName);

  auto familyOverlay = loadByFamily(family);
  if (familyOverlay.has_value()) {
    return familyOverlay;
  }
  return loadByFamily("generic");
}

void HintingLoader::bootstrapDefaults(const std::string &builtinHintingDir) {
  const std::filesystem::path builtinDir(builtinHintingDir);
  if (!std::filesystem::exists(builtinDir)) {
    return;
  }

  const std::filesystem::path userDir = PlatformPaths::firmiusHomeDir() / "hinting";
  try {
    std::filesystem::create_directories(userDir);
  } catch (const std::filesystem::filesystem_error &) {
    return;
  }

  for (const auto &entry : std::filesystem::directory_iterator(builtinDir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".md") {
      continue;
    }
    const std::filesystem::path target = userDir / entry.path().filename();
    try {
      std::filesystem::copy_file(
          entry.path(), target, std::filesystem::copy_options::overwrite_existing);
    } catch (const std::filesystem::filesystem_error &) {
      // Best effort only.
    }
  }
}

} // namespace firmius::core
