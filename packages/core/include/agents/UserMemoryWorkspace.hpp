#ifndef FIRMIUS_CORE_USER_MEMORY_WORKSPACE_HPP
#define FIRMIUS_CORE_USER_MEMORY_WORKSPACE_HPP

#include <string>

namespace firmius::core {

struct UserMemoryWorkspace {
  std::string rootDir;
  std::string userFile;
  std::string behaviorFile;
  std::string projectDir;
  std::string fixesDir;
  std::string workspaceId;
};

UserMemoryWorkspace ensureUserMemoryWorkspace(const std::string &cwd);

std::string buildUserMemoryOverlay(const std::string &cwd);

} // namespace firmius::core

#endif
