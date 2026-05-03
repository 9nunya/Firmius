#include "utils/PermissionProfiles.hpp"

#include "utils/PlatformPaths.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <rapidjson/document.h>

namespace firmius::shared {
namespace {

std::string normalizeProfileKey(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  std::replace(value.begin(), value.end(), '-', '_');
  return value;
}

struct BuiltinProfileSeed {
  const char *filename;
  const char *json;
};

constexpr BuiltinProfileSeed kBuiltinProfiles[] = {
    {"ask.json",
     R"({
  "name": "ask",
  "mode": "request",
  "title": "ASK",
  "description": "Prompt before reads, edits, or commands that need permission.",
  "examples": [
    "Read a file outside the working tree -> ask first.",
    "Run a shell command -> ask first.",
    "Write a file -> ask first."
  ]
})"},
    {"allow.json",
     R"({
  "name": "allow",
  "mode": "always_allow",
  "title": "ALLOW",
  "description": "Allow non-vulnerable permissioned actions without prompting.",
  "examples": [
    "Standard reads -> auto allow.",
    "Standard edits -> auto allow.",
    "Non-vulnerable commands -> auto allow."
  ]
})"},
    {"deny.json",
     R"({
  "name": "deny",
  "mode": "deny_all",
  "title": "DENY",
  "description": "Deny permissioned reads, edits, and commands by default.",
  "examples": [
    "Read request -> deny.",
    "Edit request -> deny.",
    "Command request -> deny."
  ]
})"},
};

} // namespace

std::filesystem::path permissionProfilesDir() {
  return PlatformPaths::firmiusHomeDir() / "permissions";
}

std::string permissionModeStorageString(ThreadPermissionMode mode) {
  switch (mode) {
  case ThreadPermissionMode::Request:
    return "request";
  case ThreadPermissionMode::AlwaysAllow:
    return "always_allow";
  case ThreadPermissionMode::DenyAll:
    return "deny_all";
  }
  return "request";
}

ThreadPermissionMode permissionModeFromStorageString(const std::string &value) {
  const std::string normalized = normalizeProfileKey(value);
  if (normalized == "always_allow" || normalized == "allow") {
    return ThreadPermissionMode::AlwaysAllow;
  }
  if (normalized == "deny_all" || normalized == "deny") {
    return ThreadPermissionMode::DenyAll;
  }
  return ThreadPermissionMode::Request;
}

void ensureBuiltinPermissionProfiles() {
  std::error_code ec;
  std::filesystem::create_directories(permissionProfilesDir(), ec);
  for (const auto &seed : kBuiltinProfiles) {
    const auto path = permissionProfilesDir() / seed.filename;
    if (std::filesystem::exists(path, ec)) {
      continue;
    }
    std::ofstream out(path);
    if (!out.is_open()) {
      continue;
    }
    out << seed.json;
  }
}

std::optional<PermissionProfile>
loadPermissionProfile(const std::string &profileName) {
  ensureBuiltinPermissionProfiles();
  const std::string canonical = canonicalPermissionProfileName(profileName)
                                    .value_or(normalizeProfileKey(profileName));
  if (canonical.empty()) {
    return std::nullopt;
  }

  const auto path = permissionProfilesDir() / (canonical + ".json");
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  if (content.empty()) {
    return std::nullopt;
  }

  rapidjson::Document doc;
  doc.Parse(content.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return std::nullopt;
  }

  PermissionProfile profile;
  profile.name = doc.HasMember("name") && doc["name"].IsString()
                     ? normalizeProfileKey(doc["name"].GetString())
                     : canonical;
  profile.mode = doc.HasMember("mode") && doc["mode"].IsString()
                     ? permissionModeFromStorageString(doc["mode"].GetString())
                     : ThreadPermissionMode::Request;
  profile.title = doc.HasMember("title") && doc["title"].IsString()
                      ? std::string(doc["title"].GetString())
                      : profile.name;
  profile.description =
      doc.HasMember("description") && doc["description"].IsString()
          ? std::string(doc["description"].GetString())
          : "";
  return profile;
}

std::optional<ThreadPermissionMode>
resolvePermissionProfileMode(const std::string &value) {
  const std::string normalized = normalizeProfileKey(value);
  if (normalized.empty()) {
    return std::nullopt;
  }
  if (normalized == "ask" || normalized == "request") {
    return ThreadPermissionMode::Request;
  }
  if (normalized == "allow" || normalized == "always_allow") {
    return ThreadPermissionMode::AlwaysAllow;
  }
  if (normalized == "deny" || normalized == "deny_all") {
    return ThreadPermissionMode::DenyAll;
  }
  if (auto profile = loadPermissionProfile(normalized)) {
    return profile->mode;
  }
  return std::nullopt;
}

std::optional<std::string>
canonicalPermissionProfileName(const std::string &value) {
  const std::string normalized = normalizeProfileKey(value);
  if (normalized.empty()) {
    return std::nullopt;
  }
  if (normalized == "request" || normalized == "ask") {
    return std::string("ask");
  }
  if (normalized == "always_allow" || normalized == "allow") {
    return std::string("allow");
  }
  if (normalized == "deny_all" || normalized == "deny") {
    return std::string("deny");
  }
  const auto path = permissionProfilesDir() / (normalized + ".json");
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return normalized;
  }
  return std::nullopt;
}

std::optional<std::string>
canonicalPermissionProfileName(ThreadPermissionMode mode) {
  switch (mode) {
  case ThreadPermissionMode::Request:
    return std::string("ask");
  case ThreadPermissionMode::AlwaysAllow:
    return std::string("allow");
  case ThreadPermissionMode::DenyAll:
    return std::string("deny");
  }
  return std::nullopt;
}

} // namespace firmius::shared
