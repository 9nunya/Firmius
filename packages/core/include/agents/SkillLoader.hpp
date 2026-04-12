#ifndef FIRMIUS_CORE_SKILL_LOADER_HPP
#define FIRMIUS_CORE_SKILL_LOADER_HPP

#include "Skill.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace firmius::core {

class SkillLoader {
public:
  static std::vector<std::string> resolveSkillsDirs(); // ~/.agents/skills
  static std::optional<std::string>
  resolveSkillRoot(const std::string_view skillId);
  static std::optional<shared::AgentSkill>
  getSkill(const std::string_view skillId);

private:
  static std::optional<std::string>
  lookForSkillBaseDir(const std::string_view skillId);
};

} // namespace firmius::core

#endif