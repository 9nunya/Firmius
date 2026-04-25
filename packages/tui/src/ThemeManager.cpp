#include "ThemeManager.hpp"
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
ftxui::Color ParseHex(const std::string &hex) {
  if (hex.empty() || hex[0] != '#')
    return ftxui::Color::Default;
  try {
    if (hex.size() == 7) {
      int r = std::stoi(hex.substr(1, 2), nullptr, 16);
      int g = std::stoi(hex.substr(3, 2), nullptr, 16);
      int b = std::stoi(hex.substr(5, 2), nullptr, 16);
      return ftxui::Color::RGB(r, g, b);
    }
  } catch (...) {
  }
  return ftxui::Color::Default;
}

ftxui::Color GetColor(const rapidjson::Value &v, const char *key) {
  if (v.HasMember(key) && v[key].IsString()) {
    return ParseHex(v[key].GetString());
  }
  return ftxui::Color::Default;
}

ColorGroup GetColorGroup(const rapidjson::Value &v) {
  ColorGroup cg;
  cg.bg = GetColor(v, "bg");
  cg.fg = GetColor(v, "fg");
  return cg;
}

StateColors GetStateColors(const rapidjson::Value &v) {
  StateColors sc;
  if (v.IsObject()) {
    if (v.HasMember("normal"))
      sc.normal = GetColorGroup(v["normal"]);
    if (v.HasMember("focused"))
      sc.focused = GetColorGroup(v["focused"]);
    if (v.HasMember("busy"))
      sc.busy = GetColorGroup(v["busy"]);
    if (v.HasMember("error"))
      sc.error = GetColorGroup(v["error"]);
    if (v.HasMember("glint") && v["glint"].IsArray()) {
      for (auto &c : v["glint"].GetArray()) {
        if (c.IsString())
          sc.glint.push_back(ParseHex(c.GetString()));
      }
    }
  }
  return sc;
}
} // namespace

ThemeManager &ThemeManager::instance() {
  static ThemeManager inst;
  return inst;
}

ThemeManager::ThemeManager() { loadThemes(); }

void ThemeManager::loadThemes() {
  themes_.clear();

  loadSystemThemes();
  loadUserThemes();

  if (themes_.empty()) {
    // Should not happen if installation is correct, but let's be safe
    // and provide at least one empty-ish theme if everything fails
    Theme fallback;
    fallback.name = "Fallback";
    themes_.push_back(fallback);
  }

  loadPersistedSelection();
}

void ThemeManager::loadSystemThemes() {
  // All themes including built-in ones are installed to ~/.firmius/themes
  // by the installation process.
}

void ThemeManager::loadUserThemes() {
  const std::filesystem::path userDir =
      firmius::shared::PlatformPaths::firmiusHomeDir() / "themes";

  if (std::filesystem::exists(userDir) &&
      std::filesystem::is_directory(userDir)) {
    for (const auto &entry : std::filesystem::directory_iterator(userDir)) {
      if (entry.path().extension() == ".json") {
        try {
          themes_.push_back(loadThemeFromFile(entry.path().string()));
        } catch (...) {
        }
      }
    }
  }
}

Theme ThemeManager::loadThemeFromFile(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open())
    throw std::runtime_error("Failed to open theme file");

  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  file.close();

  rapidjson::Document doc;
  doc.Parse(content.c_str());
  if (doc.HasParseError())
    throw std::runtime_error("JSON Parse Error");

  Theme t;
  t.name = doc.HasMember("name") ? doc["name"].GetString() : "Unnamed";

  // Base
  if (doc.HasMember("base")) {
    const auto &b = doc["base"];
    t.base.bg = GetColor(b, "bg");
    t.base.fg = GetColor(b, "fg");
    t.base.border = GetColor(b, "border");
    t.base.separator = GetColor(b, "separator");
    t.base.highlight = GetColor(b, "highlight");
    t.base.dim = GetColor(b, "dim");
  }

  // Status Bar
  if (doc.HasMember("status_bar")) {
    const auto &sb = doc["status_bar"];
    t.status_bar.idle = GetStateColors(sb["idle"]);
    t.status_bar.streaming = GetStateColors(sb["streaming"]);
    t.status_bar.executing_tool = GetStateColors(sb["executing_tool"]);
    t.status_bar.provider_waiting = GetStateColors(sb["provider_waiting"]);
    t.status_bar.compacting = GetStateColors(sb["compacting"]);
    t.status_bar.error = GetStateColors(sb["error"]);

    t.status_bar.agent_bg = GetColor(sb, "agent_bg");
    t.status_bar.agent_fg = GetColor(sb, "agent_fg");
    t.status_bar.pill_bg = GetColor(sb, "pill_bg");
    t.status_bar.pill_fg = GetColor(sb, "pill_fg");
    t.status_bar.filler_bg = GetColor(sb, "filler_bg");

    if (sb.HasMember("context")) {
      const auto &ctx = sb["context"];
      t.status_bar.context.bg = GetColor(ctx, "bg");
      t.status_bar.context.icon = GetColor(ctx, "icon");
      t.status_bar.context.low = GetColor(ctx, "low");
      t.status_bar.context.medium = GetColor(ctx, "medium");
      t.status_bar.context.high = GetColor(ctx, "high");
    }
  }

  // Agent Strip
  if (doc.HasMember("agent_strip")) {
    const auto &as = doc["agent_strip"];
    t.agent_strip.bg = GetColor(as, "bg");
    t.agent_strip.item = GetStateColors(as["item"]);

    if (as.HasMember("pills")) {
      const auto &p = as["pills"];
      t.agent_strip.pills.slug_bg = GetColor(p, "slug_bg");
      t.agent_strip.pills.slug_fg = GetColor(p, "slug_fg");
      t.agent_strip.pills.purpose_bg = GetColor(p, "purpose_bg");
      t.agent_strip.pills.purpose_fg = GetColor(p, "purpose_fg");
      t.agent_strip.pills.model_bg = GetColor(p, "model_bg");
      t.agent_strip.pills.model_fg = GetColor(p, "model_fg");
      t.agent_strip.pills.state_bg = GetColor(p, "state_bg");
      t.agent_strip.pills.state_fg = GetColor(p, "state_fg");
      t.agent_strip.pills.tool_bg = GetColor(p, "tool_bg");
      t.agent_strip.pills.tool_fg = GetColor(p, "tool_fg");
      t.agent_strip.pills.context_bg = GetColor(p, "context_bg");
    }
  }

  // Chat
  if (doc.HasMember("chat")) {
    const auto &c = doc["chat"];
    t.chat.bg = GetColor(c, "bg");
    t.chat.user_prefix = GetColor(c, "user_prefix");
    t.chat.agent_prefix = GetColor(c, "agent_prefix");
    t.chat.timestamp = GetColor(c, "timestamp");

    if (c.HasMember("markdown")) {
      const auto &md = c["markdown"];
      t.chat.markdown.text = GetColor(md, "text");
      t.chat.markdown.header = GetColor(md, "header");
      t.chat.markdown.code_bg = GetColor(md, "code_bg");
      t.chat.markdown.code_fg = GetColor(md, "code_fg");
      t.chat.markdown.link = GetColor(md, "link");
      t.chat.markdown.quote_bar = GetColor(md, "quote_bar");
      t.chat.markdown.quote_text = GetColor(md, "quote_text");
    }
  }

  // Syntax
  if (doc.HasMember("syntax")) {
    const auto &s = doc["syntax"];
    t.syntax.keyword = GetColor(s, "keyword");
    t.syntax.string = GetColor(s, "string");
    t.syntax.comment = GetColor(s, "comment");
    t.syntax.number = GetColor(s, "number");
    t.syntax.function = GetColor(s, "function");
    t.syntax.type = GetColor(s, "type");
    t.syntax.op = GetColor(s, "op");
    t.syntax.attr = GetColor(s, "attr");
    t.syntax.constant = GetColor(s, "constant");
    t.syntax.variable = GetColor(s, "variable");
    t.syntax.tag = GetColor(s, "tag");
  }

  // Tool Blocks
  if (doc.HasMember("tool_blocks")) {
    const auto &tb = doc["tool_blocks"];
    t.tool_blocks.generic_bg = GetColor(tb, "generic_bg");
    t.tool_blocks.generic_border = GetColor(tb, "generic_border");
    t.tool_blocks.generic_header_bg = GetColor(tb, "generic_header_bg");
    t.tool_blocks.generic_title = GetColor(tb, "generic_title");
    t.tool_blocks.generic_icon = GetColor(tb, "generic_icon");

    if (tb.HasMember("specific")) {
      const auto &spec = tb["specific"];
      t.tool_blocks.specific.file_read = GetColorGroup(spec["file_read"]);
      t.tool_blocks.specific.file_edit = GetColorGroup(spec["file_edit"]);
      t.tool_blocks.specific.terminal = GetColorGroup(spec["terminal"]);
      t.tool_blocks.specific.subagent = GetColorGroup(spec["subagent"]);
      t.tool_blocks.specific.ls = GetColorGroup(spec["ls"]);
      t.tool_blocks.specific.wait = GetColorGroup(spec["wait"]);
    }

    if (tb.HasMember("glint") && tb["glint"].IsArray()) {
      for (auto &c : tb["glint"].GetArray()) {
        if (c.IsString())
          t.tool_blocks.glint.push_back(ParseHex(c.GetString()));
      }
    }
  }

  // Input
  if (doc.HasMember("input")) {
    const auto &i = doc["input"];
    t.input.bg = GetColor(i, "bg");
    t.input.fg = GetColor(i, "fg");
    t.input.prompt = GetColor(i, "prompt");
    t.input.cursor = GetColor(i, "cursor");
    t.input.placeholder = GetColor(i, "placeholder");
  }

  // Modals
  if (doc.HasMember("modals")) {
    const auto &m = doc["modals"];
    t.modals.overlay = GetColor(m, "overlay");
    t.modals.bg = GetColor(m, "bg");
    t.modals.fg = GetColor(m, "fg");
    t.modals.border = GetColor(m, "border");
    t.modals.title = GetColor(m, "title");
    t.modals.highlight_bg = GetColor(m, "highlight_bg");
    t.modals.highlight_fg = GetColor(m, "highlight_fg");
    t.modals.button_bg = GetColor(m, "button_bg");
    t.modals.button_fg = GetColor(m, "button_fg");
  }

  return t;
}

void ThemeManager::cycleTheme() {
  if (themes_.empty())
    return;
  current_theme_index_ = (current_theme_index_ + 1) % themes_.size();
  persistSelection();
}

void ThemeManager::setTheme(const std::string &name) {
  for (size_t i = 0; i < themes_.size(); ++i) {
    if (themes_[i].name == name) {
      current_theme_index_ = i;
      persistSelection();
      return;
    }
  }
}

void ThemeManager::loadPersistedSelection() {
  const auto preferences = loadUserPreferences();
  if (!preferences.theme_name.has_value()) {
    return;
  }
  const std::string &wanted = *preferences.theme_name;
  for (size_t i = 0; i < themes_.size(); ++i) {
    if (themes_[i].name == wanted) {
      current_theme_index_ = i;
      return;
    }
  }
}

void ThemeManager::persistSelection() const {
  if (themes_.empty() || current_theme_index_ >= themes_.size()) {
    return;
  }
  UserPreferences preferences;
  preferences.theme_name = themes_[current_theme_index_].name;
  saveUserPreferences(preferences);
}

const Theme &ThemeManager::getCurrentTheme() const {
  return themes_[current_theme_index_];
}

std::vector<std::string> ThemeManager::getThemeNames() const {
  std::vector<std::string> names;
  for (const auto &t : themes_) {
    names.push_back(t.name);
  }
  return names;
}

} // namespace firmius::tui
