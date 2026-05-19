#ifndef FIRMIUS_CORE_HINTINGLOADER_HPP
#define FIRMIUS_CORE_HINTINGLOADER_HPP

#include <optional>
#include <string>
#include <vector>

namespace firmius::core {

struct HintingOverlay {
  std::string name;
  std::string title;
  std::string description;
  bool builtin = false;
  bool enabled = true;
  int priority = 0;
  std::string body;
  std::string sourcePath;
};

class ModelHintResolver {
public:
  static std::string detectFamily(const std::string &providerId,
                                  const std::string &modelId,
                                  const std::string &variantName);
};

class HintingLoader {
public:
  static std::string resolveHintingDir();
  static std::optional<HintingOverlay>
  loadForModel(const std::string &providerId, const std::string &modelId,
               const std::string &variantName);
  static std::optional<HintingOverlay> loadByFamily(const std::string &family);
  static void bootstrapDefaults(const std::string &builtinHintingDir);

private:
  static std::optional<HintingOverlay>
  loadFromPath(const std::string &family, const std::string &path);
  static std::vector<std::string> resolveHintingDirs();
};

} // namespace firmius::core

#endif
