#ifndef FIRMIUS_SHARED_SKILL_HPP
#define FIRMIUS_SHARED_SKILL_HPP

#include <string>

namespace firmius::shared {

struct AgentSkill {
  std::string skillId;
  std::string skillRoot;
  std::string skillMdPath;
  std::string skillWd; // legacy alias for skillRoot
  std::string name;
  std::string description;
  std::string entrypointBody;
};

} // namespace firmius::shared

#endif