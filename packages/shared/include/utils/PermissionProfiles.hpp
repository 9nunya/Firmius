#ifndef FIRMIUS_SHARED_PERMISSIONPROFILES_HPP
#define FIRMIUS_SHARED_PERMISSIONPROFILES_HPP

#include "Enums.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace firmius::shared {

struct PermissionProfile {
  std::string name;
  ThreadPermissionMode mode = ThreadPermissionMode::Request;
  std::string title;
  std::string description;
};

std::filesystem::path permissionProfilesDir();
void ensureBuiltinPermissionProfiles();
std::optional<PermissionProfile>
loadPermissionProfile(const std::string &profileName);
std::optional<ThreadPermissionMode>
resolvePermissionProfileMode(const std::string &value);
std::optional<std::string>
canonicalPermissionProfileName(const std::string &value);
std::optional<std::string>
canonicalPermissionProfileName(ThreadPermissionMode mode);
std::string permissionModeStorageString(ThreadPermissionMode mode);
ThreadPermissionMode permissionModeFromStorageString(const std::string &value);

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_PERMISSIONPROFILES_HPP
