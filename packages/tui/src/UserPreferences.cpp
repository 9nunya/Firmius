#include "UserPreferences.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::tui {

namespace {

std::filesystem::path preferencesPath() {
  const char *home = std::getenv("HOME");
  std::filesystem::path base = home ? home : "/root";
  base /= ".firmius";
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  return base / "preferences.json";
}

std::string permissionModeToString(shared::ThreadPermissionMode mode) {
  switch (mode) {
  case shared::ThreadPermissionMode::Request:
    return "request";
  case shared::ThreadPermissionMode::AlwaysAllow:
    return "always_allow";
  case shared::ThreadPermissionMode::DenyAll:
    return "deny_all";
  }
  return "request";
}

shared::ThreadPermissionMode permissionModeFromString(
    const std::string &value) {
  if (value == "always_allow") {
    return shared::ThreadPermissionMode::AlwaysAllow;
  }
  if (value == "deny_all") {
    return shared::ThreadPermissionMode::DenyAll;
  }
  return shared::ThreadPermissionMode::Request;
}

rapidjson::Document loadPreferencesDocument() {
  rapidjson::Document doc;
  doc.SetObject();

  std::ifstream file(preferencesPath());
  if (!file.is_open()) {
    return doc;
  }

  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  if (content.empty()) {
    return doc;
  }

  doc.Parse(content.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    doc.SetObject();
  }
  return doc;
}

std::optional<std::string> loadLegacyThemeSelection() {
  const char *home = std::getenv("HOME");
  std::filesystem::path legacyPath = home ? home : "/root";
  legacyPath /= ".firmius/theme_selection.json";

  std::ifstream file(legacyPath);
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
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("theme") ||
      !doc["theme"].IsString()) {
    return std::nullopt;
  }
  return std::string(doc["theme"].GetString());
}

} // namespace

UserPreferences loadUserPreferences() {
  UserPreferences preferences;
  rapidjson::Document doc = loadPreferencesDocument();

  if (doc.HasMember("theme") && doc["theme"].IsString()) {
    preferences.theme_name = std::string(doc["theme"].GetString());
  } else if (auto legacyTheme = loadLegacyThemeSelection()) {
    preferences.theme_name = *legacyTheme;
    saveUserPreferences(preferences);
  }
  if (doc.HasMember("preferred_permission_mode") &&
      doc["preferred_permission_mode"].IsString()) {
    preferences.preferred_permission_mode = permissionModeFromString(
        std::string(doc["preferred_permission_mode"].GetString()));
  }
  if (doc.HasMember("prefer_todo_panel_on_narrow") &&
      doc["prefer_todo_panel_on_narrow"].IsBool()) {
    preferences.prefer_todo_panel_on_narrow =
        doc["prefer_todo_panel_on_narrow"].GetBool();
  }

  return preferences;
}

void saveUserPreferences(const UserPreferences &preferences) {
  rapidjson::Document doc = loadPreferencesDocument();
  auto &alloc = doc.GetAllocator();

  if (preferences.theme_name.has_value()) {
    rapidjson::Value key("theme", alloc);
    rapidjson::Value value(preferences.theme_name->c_str(), alloc);
    if (doc.HasMember("theme")) {
      doc["theme"] = value;
    } else {
      doc.AddMember(key, value, alloc);
    }
  }

  if (preferences.preferred_permission_mode.has_value()) {
    const std::string mode =
        permissionModeToString(*preferences.preferred_permission_mode);
    rapidjson::Value value(mode.c_str(), alloc);
    if (doc.HasMember("preferred_permission_mode")) {
      doc["preferred_permission_mode"] = value;
    } else {
      doc.AddMember("preferred_permission_mode", value, alloc);
    }
  }

  if (preferences.prefer_todo_panel_on_narrow.has_value()) {
    if (doc.HasMember("prefer_todo_panel_on_narrow")) {
      doc["prefer_todo_panel_on_narrow"] =
          preferences.prefer_todo_panel_on_narrow.value();
    } else {
      doc.AddMember("prefer_todo_panel_on_narrow",
                    preferences.prefer_todo_panel_on_narrow.value(), alloc);
    }
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  std::ofstream out(preferencesPath());
  if (!out.is_open()) {
    return;
  }
  out << buffer.GetString();
}

} // namespace firmius::tui
