#include "UserPreferences.hpp"
#include "utils/PlatformPaths.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::tui {

namespace {

std::filesystem::path preferencesPath() {
  std::filesystem::path base = firmius::shared::PlatformPaths::firmiusHomeDir();
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
  const std::filesystem::path legacyPath =
      firmius::shared::PlatformPaths::firmiusHomeDir() /
      "theme_selection.json";

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

std::optional<bool> ReadBool(const rapidjson::Value &value, const char *key) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsBool()) {
    return value[key].GetBool();
  }
  return std::nullopt;
}

std::optional<int> ReadInt(const rapidjson::Value &value, const char *key) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsInt()) {
    return value[key].GetInt();
  }
  return std::nullopt;
}

std::optional<std::string> ReadString(const rapidjson::Value &value,
                                      const char *key) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsString()) {
    return std::string(value[key].GetString());
  }
  return std::nullopt;
}

void WriteBool(rapidjson::Value &object, const char *key, bool value,
               rapidjson::Document::AllocatorType &alloc) {
  if (object.HasMember(key)) {
    object[key] = value;
  } else {
    object.AddMember(rapidjson::Value(key, alloc).Move(), value, alloc);
  }
}

void WriteInt(rapidjson::Value &object, const char *key, int value,
              rapidjson::Document::AllocatorType &alloc) {
  if (object.HasMember(key)) {
    object[key] = value;
  } else {
    object.AddMember(rapidjson::Value(key, alloc).Move(), value, alloc);
  }
}

void WriteString(rapidjson::Value &object, const char *key,
                 const std::string &value,
                 rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value string_value(value.c_str(), alloc);
  if (object.HasMember(key)) {
    object[key] = string_value;
  } else {
    object.AddMember(rapidjson::Value(key, alloc).Move(), string_value, alloc);
  }
}

SkinConfig ReadSkinConfigObject(const rapidjson::Value &value, SkinKind kind) {
  SkinConfig cfg = defaultSkinConfig(kind);
  cfg.kind = kind;

  if (!value.IsObject()) {
    return cfg;
  }

  if (auto flag = ReadBool(value, "show_agent_strip")) {
    cfg.show_agent_strip = *flag;
  }
  if (auto flag = ReadBool(value, "show_work_panel")) {
    cfg.show_work_panel = *flag;
  }
  if (auto flag = ReadBool(value, "show_title_bar")) {
    cfg.show_title_bar = *flag;
  }
  if (auto flag = ReadBool(value, "show_errors")) {
    cfg.show_errors = *flag;
  }
  if (auto flag = ReadBool(value, "show_notices")) {
    cfg.show_notices = *flag;
  }
  if (auto flag = ReadBool(value, "show_turn_numbers")) {
    cfg.show_turn_numbers = *flag;
  }
  if (auto flag = ReadBool(value, "show_turn_footers")) {
    cfg.show_turn_footers = *flag;
  }
  if (auto flag = ReadBool(value, "show_turn_timing")) {
    cfg.show_turn_timing = *flag;
  }
  if (auto flag = ReadBool(value, "show_turn_tokens")) {
    cfg.show_turn_tokens = *flag;
  }
  if (auto flag = ReadBool(value, "show_live_footer")) {
    cfg.show_live_footer = *flag;
  }
  if (auto flag = ReadBool(value, "show_blank_lines")) {
    cfg.show_blank_lines = *flag;
  }
  if (auto flag = ReadBool(value, "blank_lines_between_messages")) {
    cfg.blank_lines_between_messages = *flag;
  }
  if (auto flag = ReadBool(value, "blank_lines_after_user")) {
    cfg.blank_lines_after_user = *flag;
  }
  if (auto flag = ReadBool(value, "blank_lines_after_agent")) {
    cfg.blank_lines_after_agent = *flag;
  }
  if (auto flag = ReadBool(value, "blank_lines_after_tools")) {
    cfg.blank_lines_after_tools = *flag;
  }
  if (auto flag = ReadBool(value, "show_thinking_blocks")) {
    cfg.show_thinking_blocks = *flag;
  }
  if (auto flag = ReadBool(value, "show_thinking")) {
    cfg.show_thinking = *flag;
  }
  if (auto flag = ReadBool(value, "show_thinking_label")) {
    cfg.show_thinking_label = *flag;
  }
  if (auto flag = ReadBool(value, "show_user_bubble_bg")) {
    cfg.show_user_bubble_bg = *flag;
  }
  if (auto flag = ReadBool(value, "indent_agent_rows")) {
    cfg.indent_agent_rows = *flag;
  }
  if (auto flag = ReadBool(value, "show_compaction_markers")) {
    cfg.show_compaction_markers = *flag;
  }
  if (auto flag = ReadBool(value, "show_persistent_live_row")) {
    cfg.show_persistent_live_row = *flag;
  }
  if (auto flag = ReadBool(value, "live_row_busy_only")) {
    cfg.live_row_busy_only = *flag;
  }
  if (auto flag = ReadBool(value, "live_row_glint")) {
    cfg.live_row_glint = *flag;
  }
  if (auto flag = ReadBool(value, "live_row_gradient")) {
    cfg.live_row_gradient = *flag;
  }
  if (auto flag = ReadBool(value, "live_row_show_elapsed")) {
    cfg.live_row_show_elapsed = *flag;
  }
  if (auto flag = ReadBool(value, "live_row_show_activity")) {
    cfg.live_row_show_activity = *flag;
  }
  if (auto flag = ReadBool(value, "live_row_show_plan_excerpt")) {
    cfg.live_row_show_plan_excerpt = *flag;
  }
  if (auto flag = ReadBool(value, "live_row_show_todo_excerpt")) {
    cfg.live_row_show_todo_excerpt = *flag;
  }
  if (auto seconds = ReadInt(value, "live_row_cycle_seconds")) {
    cfg.live_row_cycle_seconds = *seconds;
  }
  if (auto mode = ReadString(value, "live_row_phrase_bank")) {
    cfg.live_row_phrase_bank = *mode;
  }
  if (auto mode = ReadString(value, "live_row_mode")) {
    cfg.live_row_mode = *mode;
  }
  if (auto flag = ReadBool(value, "compact_input")) {
    cfg.compact_input = *flag;
  }
  if (auto flag = ReadBool(value, "show_token_counts")) {
    cfg.show_token_counts = *flag;
  }
  if (auto flag = ReadBool(value, "status_show_processes")) {
    cfg.status_show_processes = *flag;
  }
  if (auto flag = ReadBool(value, "dim_tool_metadata")) {
    cfg.dim_tool_metadata = *flag;
  }
  if (auto flag = ReadBool(value, "show_tool_borders")) {
    cfg.show_tool_borders = *flag;
  }
  if (auto flag = ReadBool(value, "show_tool_headers")) {
    cfg.show_tool_headers = *flag;
  }
  if (auto flag = ReadBool(value, "show_tool_body")) {
    cfg.show_tool_body = *flag;
  }
  if (auto flag = ReadBool(value, "show_tool_icons")) {
    cfg.show_tool_icons = *flag;
  }
  if (auto flag = ReadBool(value, "show_tool_status_dots")) {
    cfg.show_tool_status_dots = *flag;
  }
  if (auto flag = ReadBool(value, "glint_tool_icons")) {
    cfg.glint_tool_icons = *flag;
  }
  if (auto flag = ReadBool(value, "glint_tool_blocks")) {
    cfg.glint_tool_blocks = *flag;
  }
  if (auto flag = ReadBool(value, "glint_quick_tools")) {
    cfg.glint_quick_tools = *flag;
  }
  if (auto flag = ReadBool(value, "glint_enabled")) {
    cfg.glint_enabled = *flag;
  }
  if (auto flag = ReadBool(value, "glint_status_bar")) {
    cfg.glint_status_bar = *flag;
  }
  if (auto flag = ReadBool(value, "show_plan_inline")) {
    cfg.show_plan_inline = *flag;
  }
  if (auto flag = ReadBool(value, "show_todo_inline")) {
    cfg.show_todo_inline = *flag;
  }
  if (auto flag = ReadBool(value, "claudex_hide_done_footer")) {
    cfg.claudex_hide_done_footer = *flag;
  }
  if (auto flag = ReadBool(value, "claudex_diffs_collapsed_by_default")) {
    cfg.claudex_diffs_collapsed_by_default = *flag;
  }
  if (auto lines = ReadInt(value, "tool_result_preview_lines")) {
    cfg.tool_result_preview_lines = *lines;
  }
  if (auto lines = ReadInt(value, "process_output_lines")) {
    cfg.process_output_lines = *lines;
  }
  if (auto mode = ReadString(value, "tool_display")) {
    cfg.tool_display = toolDisplayModeFromString(*mode, cfg.tool_display);
  }
  if (auto mode = ReadString(value, "status_bar_mode")) {
    cfg.status_bar_mode = statusBarModeFromString(*mode, cfg.status_bar_mode);
  }
  if (auto mode = ReadString(value, "diffs_default")) {
    cfg.diffs_default = diffDefaultModeFromString(*mode, cfg.diffs_default);
  }
  if (auto mode = ReadString(value, "tool_results")) {
    cfg.tool_results = toolResultsModeFromString(*mode, cfg.tool_results);
  }
  if (auto mode = ReadString(value, "quick_tools_display")) {
    cfg.quick_tools_display =
        quickToolsDisplayModeFromString(*mode, cfg.quick_tools_display);
  }
  if (auto mode = ReadString(value, "glint_speed")) {
    cfg.glint_speed = glintSpeedFromString(*mode, cfg.glint_speed);
  }
  if (auto mode = ReadString(value, "spinner_style")) {
    cfg.spinner_style = spinnerStyleFromString(*mode, cfg.spinner_style);
  }

  return cfg;
}

void WriteSkinConfigObject(rapidjson::Value &object, const SkinConfig &cfg,
                           rapidjson::Document::AllocatorType &alloc) {
  object.SetObject();
  WriteBool(object, "show_agent_strip", cfg.show_agent_strip, alloc);
  WriteBool(object, "show_work_panel", cfg.show_work_panel, alloc);
  WriteBool(object, "show_title_bar", cfg.show_title_bar, alloc);
  WriteBool(object, "show_errors", cfg.show_errors, alloc);
  WriteBool(object, "show_notices", cfg.show_notices, alloc);
  WriteBool(object, "show_turn_numbers", cfg.show_turn_numbers, alloc);
  WriteBool(object, "show_turn_footers", cfg.show_turn_footers, alloc);
  WriteBool(object, "show_turn_timing", cfg.show_turn_timing, alloc);
  WriteBool(object, "show_turn_tokens", cfg.show_turn_tokens, alloc);
  WriteBool(object, "show_live_footer", cfg.show_live_footer, alloc);
  WriteBool(object, "show_blank_lines", cfg.show_blank_lines, alloc);
  WriteBool(object, "blank_lines_between_messages",
            cfg.blank_lines_between_messages, alloc);
  WriteBool(object, "blank_lines_after_user", cfg.blank_lines_after_user,
            alloc);
  WriteBool(object, "blank_lines_after_agent", cfg.blank_lines_after_agent,
            alloc);
  WriteBool(object, "blank_lines_after_tools", cfg.blank_lines_after_tools,
            alloc);
  WriteBool(object, "show_thinking_blocks", cfg.show_thinking_blocks, alloc);
  WriteBool(object, "show_thinking", cfg.show_thinking, alloc);
  WriteBool(object, "show_thinking_label", cfg.show_thinking_label, alloc);
  WriteBool(object, "show_user_bubble_bg", cfg.show_user_bubble_bg, alloc);
  WriteBool(object, "indent_agent_rows", cfg.indent_agent_rows, alloc);
  WriteBool(object, "show_compaction_markers", cfg.show_compaction_markers,
            alloc);
  WriteBool(object, "show_persistent_live_row", cfg.show_persistent_live_row,
            alloc);
  WriteBool(object, "live_row_busy_only", cfg.live_row_busy_only, alloc);
  WriteBool(object, "live_row_glint", cfg.live_row_glint, alloc);
  WriteBool(object, "live_row_gradient", cfg.live_row_gradient, alloc);
  WriteBool(object, "live_row_show_elapsed", cfg.live_row_show_elapsed,
            alloc);
  WriteBool(object, "live_row_show_activity", cfg.live_row_show_activity,
            alloc);
  WriteBool(object, "live_row_show_plan_excerpt",
            cfg.live_row_show_plan_excerpt, alloc);
  WriteBool(object, "live_row_show_todo_excerpt",
            cfg.live_row_show_todo_excerpt, alloc);
  WriteString(object, "live_row_phrase_bank", cfg.live_row_phrase_bank, alloc);
  WriteString(object, "live_row_mode", cfg.live_row_mode, alloc);
  WriteInt(object, "live_row_cycle_seconds", cfg.live_row_cycle_seconds,
           alloc);
  WriteBool(object, "compact_input", cfg.compact_input, alloc);
  WriteBool(object, "show_token_counts", cfg.show_token_counts, alloc);
  WriteBool(object, "status_show_processes", cfg.status_show_processes, alloc);
  WriteBool(object, "dim_tool_metadata", cfg.dim_tool_metadata, alloc);
  WriteBool(object, "show_tool_borders", cfg.show_tool_borders, alloc);
  WriteBool(object, "show_tool_headers", cfg.show_tool_headers, alloc);
  WriteBool(object, "show_tool_body", cfg.show_tool_body, alloc);
  WriteBool(object, "show_tool_icons", cfg.show_tool_icons, alloc);
  WriteBool(object, "show_tool_status_dots", cfg.show_tool_status_dots,
            alloc);
  WriteBool(object, "glint_tool_icons", cfg.glint_tool_icons, alloc);
  WriteBool(object, "glint_tool_blocks", cfg.glint_tool_blocks, alloc);
  WriteBool(object, "glint_quick_tools", cfg.glint_quick_tools, alloc);
  WriteBool(object, "glint_enabled", cfg.glint_enabled, alloc);
  WriteBool(object, "glint_status_bar", cfg.glint_status_bar, alloc);
  WriteBool(object, "show_plan_inline", cfg.show_plan_inline, alloc);
  WriteBool(object, "show_todo_inline", cfg.show_todo_inline, alloc);
  WriteString(object, "tool_display", toolDisplayModeToString(cfg.tool_display),
              alloc);
  WriteString(object, "tool_results", toolResultsModeToString(cfg.tool_results),
              alloc);
  WriteString(object, "status_bar_mode",
              statusBarModeToString(cfg.status_bar_mode), alloc);
  WriteString(object, "diffs_default",
              diffDefaultModeToString(cfg.diffs_default), alloc);
  WriteString(object, "quick_tools_display",
              quickToolsDisplayModeToString(cfg.quick_tools_display), alloc);
  WriteString(object, "glint_speed", glintSpeedToString(cfg.glint_speed),
              alloc);
  WriteString(object, "spinner_style", spinnerStyleToString(cfg.spinner_style),
              alloc);
  WriteInt(object, "tool_result_preview_lines", cfg.tool_result_preview_lines,
           alloc);
  WriteInt(object, "process_output_lines", cfg.process_output_lines, alloc);
  WriteBool(object, "claudex_hide_done_footer", cfg.claudex_hide_done_footer,
            alloc);
  WriteBool(object, "claudex_diffs_collapsed_by_default",
            cfg.claudex_diffs_collapsed_by_default, alloc);
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
  if (doc.HasMember("skin") && doc["skin"].IsString()) {
    preferences.skin_kind =
        skinKindFromString(std::string(doc["skin"].GetString()));
  }
  if (doc.HasMember("firmius") && doc["firmius"].IsObject()) {
    preferences.firmius_skin =
        ReadSkinConfigObject(doc["firmius"], SkinKind::Firmius);
  }
  if (doc.HasMember("claudex") && doc["claudex"].IsObject()) {
    preferences.claudex_skin =
        ReadSkinConfigObject(doc["claudex"], SkinKind::Claudex);
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
  if (doc.HasMember("show_agent_strip") && doc["show_agent_strip"].IsBool()) {
    preferences.show_agent_strip = doc["show_agent_strip"].GetBool();
  }
  if (doc.HasMember("show_work_panel") && doc["show_work_panel"].IsBool()) {
    preferences.show_work_panel = doc["show_work_panel"].GetBool();
  }
  if (doc.HasMember("show_persistent_live_row") &&
      doc["show_persistent_live_row"].IsBool()) {
    preferences.show_persistent_live_row =
        doc["show_persistent_live_row"].GetBool();
  }
  if (doc.HasMember("compact_status_bar") &&
      doc["compact_status_bar"].IsBool()) {
    preferences.compact_status_bar = doc["compact_status_bar"].GetBool();
  }
  if (doc.HasMember("compact_tool_display") &&
      doc["compact_tool_display"].IsBool()) {
    preferences.compact_tool_display = doc["compact_tool_display"].GetBool();
  }
  if (doc.HasMember("agent_strip_rows") && doc["agent_strip_rows"].IsInt()) {
    preferences.agent_strip_rows = doc["agent_strip_rows"].GetInt();
  }
  if (doc.HasMember("work_panel_height") && doc["work_panel_height"].IsInt()) {
    preferences.work_panel_height = doc["work_panel_height"].GetInt();
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

  if (preferences.skin_kind.has_value()) {
    const std::string skin = skinKindToString(*preferences.skin_kind);
    rapidjson::Value value(skin.c_str(), alloc);
    if (doc.HasMember("skin")) {
      doc["skin"] = value;
    } else {
      doc.AddMember("skin", value, alloc);
    }
  }

  if (preferences.firmius_skin.has_value()) {
    rapidjson::Value value;
    WriteSkinConfigObject(value, *preferences.firmius_skin, alloc);
    if (doc.HasMember("firmius")) {
      doc["firmius"] = value;
    } else {
      doc.AddMember("firmius", value, alloc);
    }
  }

  if (preferences.claudex_skin.has_value()) {
    rapidjson::Value value;
    WriteSkinConfigObject(value, *preferences.claudex_skin, alloc);
    if (doc.HasMember("claudex")) {
      doc["claudex"] = value;
    } else {
      doc.AddMember("claudex", value, alloc);
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

  if (preferences.show_agent_strip.has_value()) {
    if (doc.HasMember("show_agent_strip")) {
      doc["show_agent_strip"] = preferences.show_agent_strip.value();
    } else {
      doc.AddMember("show_agent_strip", preferences.show_agent_strip.value(),
                    alloc);
    }
  }

  if (preferences.show_work_panel.has_value()) {
    if (doc.HasMember("show_work_panel")) {
      doc["show_work_panel"] = preferences.show_work_panel.value();
    } else {
      doc.AddMember("show_work_panel", preferences.show_work_panel.value(),
                    alloc);
    }
  }

  if (preferences.show_persistent_live_row.has_value()) {
    if (doc.HasMember("show_persistent_live_row")) {
      doc["show_persistent_live_row"] =
          preferences.show_persistent_live_row.value();
    } else {
      doc.AddMember("show_persistent_live_row",
                    preferences.show_persistent_live_row.value(), alloc);
    }
  }

  if (preferences.compact_status_bar.has_value()) {
    if (doc.HasMember("compact_status_bar")) {
      doc["compact_status_bar"] = preferences.compact_status_bar.value();
    } else {
      doc.AddMember("compact_status_bar",
                    preferences.compact_status_bar.value(), alloc);
    }
  }

  if (preferences.compact_tool_display.has_value()) {
    if (doc.HasMember("compact_tool_display")) {
      doc["compact_tool_display"] = preferences.compact_tool_display.value();
    } else {
      doc.AddMember("compact_tool_display",
                    preferences.compact_tool_display.value(), alloc);
    }
  }

  if (preferences.agent_strip_rows.has_value()) {
    if (doc.HasMember("agent_strip_rows")) {
      doc["agent_strip_rows"] = preferences.agent_strip_rows.value();
    } else {
      doc.AddMember("agent_strip_rows", preferences.agent_strip_rows.value(),
                    alloc);
    }
  }

  if (preferences.work_panel_height.has_value()) {
    if (doc.HasMember("work_panel_height")) {
      doc["work_panel_height"] = preferences.work_panel_height.value();
    } else {
      doc.AddMember("work_panel_height", preferences.work_panel_height.value(),
                    alloc);
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
