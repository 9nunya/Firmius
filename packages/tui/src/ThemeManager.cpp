#include "ThemeManager.hpp"

#include "utils/PlatformPaths.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

namespace firmius::tui {

namespace {

using firmius::shared::PlatformPaths;

ThemeRgb parseHex(const std::string& hex, ThemeRgb fallback = {}) {
  if (hex.size() != 7 || hex[0] != '#') return fallback;
  try {
    return {
        std::stoi(hex.substr(1, 2), nullptr, 16),
        std::stoi(hex.substr(3, 2), nullptr, 16),
        std::stoi(hex.substr(5, 2), nullptr, 16),
    };
  } catch (...) {
    return fallback;
  }
}

ThemeRgb getColor(const rapidjson::Value& v, const char* key, ThemeRgb fallback = {}) {
  if (v.IsObject() && v.HasMember(key) && v[key].IsString()) {
    return parseHex(v[key].GetString(), fallback);
  }
  return fallback;
}

ThemeRgb mix(ThemeRgb a, ThemeRgb b, double t) {
  const double clamped = std::clamp(t, 0.0, 1.0);
  return {
      static_cast<int>(a.r + (b.r - a.r) * clamped),
      static_cast<int>(a.g + (b.g - a.g) * clamped),
      static_cast<int>(a.b + (b.b - a.b) * clamped),
  };
}

ThemeRgb deriveUserMessageBg(const ThemeSpec& theme) {
  const auto accentTint = mix(theme.chat.bg, theme.base.highlight, 0.10);
  return mix(accentTint, theme.base.separator, 0.20);
}

ThemeColorGroup getColorGroup(const rapidjson::Value& v,
                              ThemeColorGroup fallback = {}) {
  ThemeColorGroup cg = fallback;
  if (v.IsObject()) {
    cg.bg = getColor(v, "bg", cg.bg);
    cg.fg = getColor(v, "fg", cg.fg);
  }
  return cg;
}

ThemeStateColors getStateColors(const rapidjson::Value& v,
                                ThemeStateColors fallback = {}) {
  ThemeStateColors sc = fallback;
  if (!v.IsObject()) return sc;
  if (v.HasMember("normal")) sc.normal = getColorGroup(v["normal"], sc.normal);
  if (v.HasMember("focused")) sc.focused = getColorGroup(v["focused"], sc.focused);
  if (v.HasMember("busy")) sc.busy = getColorGroup(v["busy"], sc.busy);
  if (v.HasMember("error")) sc.error = getColorGroup(v["error"], sc.error);
  if (v.HasMember("glint") && v["glint"].IsArray()) {
    sc.glint.clear();
    for (const auto& entry : v["glint"].GetArray()) {
      if (entry.IsString()) sc.glint.push_back(parseHex(entry.GetString()));
    }
  }
  return sc;
}

std::filesystem::path preferencesPath() {
  return PlatformPaths::firmiusHomeDir() / "preferences.json";
}

ThemeSpec defaultTheme() {
  ThemeSpec theme;
  theme.statusBar.idle.normal = {{36, 88, 58}, {18, 20, 28}};
  theme.statusBar.streaming.normal = {{75, 143, 84}, {18, 20, 28}};
  theme.statusBar.executingTool.normal = {{201, 136, 52}, {19, 20, 24}};
  theme.statusBar.providerWaiting.normal = {{117, 164, 255}, {18, 20, 28}};
  theme.statusBar.compacting.normal = {{176, 124, 255}, {18, 20, 28}};
  theme.statusBar.error.normal = {{170, 60, 60}, {245, 245, 245}};
  theme.agentStrip.item.focused = {{117, 164, 255}, {18, 20, 28}};
  theme.agentStrip.item.busy = {{245, 194, 103}, {19, 20, 24}};
  theme.agentStrip.item.error = {{170, 60, 60}, {245, 245, 245}};
  theme.agentStrip.item.normal = {{18, 18, 28}, {136, 145, 166}};
  return theme;
}

std::optional<std::string> loadSelectedThemeName() {
  for (const auto& path : {preferencesPath(), PlatformPaths::firmiusHomeDir() / "config.json"}) {
    std::ifstream file(path);
    if (!file.is_open()) continue;
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    rapidjson::Document doc;
    doc.Parse(content.c_str());
    if (doc.IsObject() && doc.HasMember("theme") && doc["theme"].IsString()) {
      return std::string(doc["theme"].GetString());
    }
  }
  return std::nullopt;
}

void persistSelectedThemeName(const std::string& name) {
  std::filesystem::create_directories(preferencesPath().parent_path());
  rapidjson::Document doc;
  {
    std::ifstream in(preferencesPath());
    if (in.is_open()) {
      std::string content((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
      doc.Parse(content.c_str());
    }
  }
  if (!doc.IsObject()) {
    doc.SetObject();
  }
  auto& alloc = doc.GetAllocator();
  rapidjson::Value value(name.c_str(), alloc);
  if (doc.HasMember("theme")) {
    doc["theme"] = value;
  } else {
    doc.AddMember("theme", value, alloc);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  std::ofstream out(preferencesPath(), std::ios::trunc);
  out << buffer.GetString();
}

ThemeSpec loadThemeFile(const std::filesystem::path& path) {
  ThemeSpec theme = defaultTheme();
  std::ifstream file(path);
  if (!file.is_open()) return theme;
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  rapidjson::Document doc;
  doc.Parse(content.c_str());
  if (!doc.IsObject()) return theme;

  if (doc.HasMember("name") && doc["name"].IsString()) {
    theme.name = doc["name"].GetString();
  }
  if (doc.HasMember("base")) {
    const auto& base = doc["base"];
    theme.base.bg = getColor(base, "bg", theme.base.bg);
    theme.base.fg = getColor(base, "fg", theme.base.fg);
    theme.base.border = getColor(base, "border", theme.base.border);
    theme.base.separator = getColor(base, "separator", theme.base.separator);
    theme.base.highlight = getColor(base, "highlight", theme.base.highlight);
    theme.base.dim = getColor(base, "dim", theme.base.dim);
  }
  if (doc.HasMember("status_bar")) {
    const auto& sb = doc["status_bar"];
    theme.statusBar.idle = getStateColors(sb["idle"], theme.statusBar.idle);
    theme.statusBar.streaming =
        getStateColors(sb["streaming"], theme.statusBar.streaming);
    theme.statusBar.executingTool =
        getStateColors(sb["executing_tool"], theme.statusBar.executingTool);
    theme.statusBar.providerWaiting =
        getStateColors(sb["provider_waiting"], theme.statusBar.providerWaiting);
    theme.statusBar.compacting =
        getStateColors(sb["compacting"], theme.statusBar.compacting);
    theme.statusBar.error = getStateColors(sb["error"], theme.statusBar.error);
    theme.statusBar.agentBg = getColor(sb, "agent_bg", theme.statusBar.agentBg);
    theme.statusBar.agentFg = getColor(sb, "agent_fg", theme.statusBar.agentFg);
    theme.statusBar.pillBg = getColor(sb, "pill_bg", theme.statusBar.pillBg);
    theme.statusBar.pillFg = getColor(sb, "pill_fg", theme.statusBar.pillFg);
    theme.statusBar.fillerBg =
        getColor(sb, "filler_bg", theme.statusBar.fillerBg);
    if (sb.HasMember("context")) {
      const auto& ctx = sb["context"];
      theme.statusBar.context.bg =
          getColor(ctx, "bg", theme.statusBar.context.bg);
      theme.statusBar.context.icon =
          getColor(ctx, "icon", theme.statusBar.context.icon);
      theme.statusBar.context.low =
          getColor(ctx, "low", theme.statusBar.context.low);
      theme.statusBar.context.medium =
          getColor(ctx, "medium", theme.statusBar.context.medium);
      theme.statusBar.context.high =
          getColor(ctx, "high", theme.statusBar.context.high);
    }
  }
  if (doc.HasMember("agent_strip")) {
    const auto& strip = doc["agent_strip"];
    theme.agentStrip.bg = getColor(strip, "bg", theme.agentStrip.bg);
    if (strip.HasMember("item")) {
      theme.agentStrip.item =
          getStateColors(strip["item"], theme.agentStrip.item);
    }
    if (strip.HasMember("pills")) {
      const auto& pills = strip["pills"];
      theme.agentStrip.pills.slugBg =
          getColor(pills, "slug_bg", theme.agentStrip.pills.slugBg);
      theme.agentStrip.pills.slugFg =
          getColor(pills, "slug_fg", theme.agentStrip.pills.slugFg);
      theme.agentStrip.pills.purposeBg =
          getColor(pills, "purpose_bg", theme.agentStrip.pills.purposeBg);
      theme.agentStrip.pills.purposeFg =
          getColor(pills, "purpose_fg", theme.agentStrip.pills.purposeFg);
      theme.agentStrip.pills.modelBg =
          getColor(pills, "model_bg", theme.agentStrip.pills.modelBg);
      theme.agentStrip.pills.modelFg =
          getColor(pills, "model_fg", theme.agentStrip.pills.modelFg);
      theme.agentStrip.pills.stateBg =
          getColor(pills, "state_bg", theme.agentStrip.pills.stateBg);
      theme.agentStrip.pills.stateFg =
          getColor(pills, "state_fg", theme.agentStrip.pills.stateFg);
      theme.agentStrip.pills.toolBg =
          getColor(pills, "tool_bg", theme.agentStrip.pills.toolBg);
      theme.agentStrip.pills.toolFg =
          getColor(pills, "tool_fg", theme.agentStrip.pills.toolFg);
      theme.agentStrip.pills.contextBg =
          getColor(pills, "context_bg", theme.agentStrip.pills.contextBg);
    }
  }
  if (doc.HasMember("input")) {
    const auto& input = doc["input"];
    theme.input.bg = getColor(input, "bg", theme.input.bg);
    theme.input.fg = getColor(input, "fg", theme.input.fg);
    theme.input.prompt = getColor(input, "prompt", theme.input.prompt);
    theme.input.cursor = getColor(input, "cursor", theme.input.cursor);
    theme.input.placeholder =
        getColor(input, "placeholder", theme.input.placeholder);
  }
  if (doc.HasMember("chat")) {
    const auto& chat = doc["chat"];
    theme.chat.bg = getColor(chat, "bg", theme.chat.bg);
    theme.chat.userPrefix =
        getColor(chat, "user_prefix", theme.chat.userPrefix);
    theme.chat.agentPrefix =
        getColor(chat, "agent_prefix", theme.chat.agentPrefix);
    theme.chat.timestamp =
        getColor(chat, "timestamp", theme.chat.timestamp);
    theme.chat.userFg = getColor(chat, "user_fg", theme.chat.userFg);
  }
  theme.chat.userBg = deriveUserMessageBg(theme);
  if (doc.HasMember("chat")) {
    const auto& chat = doc["chat"];
    if (chat.IsObject() && chat.HasMember("user_bg") && chat["user_bg"].IsString()) {
      theme.chat.userBg = parseHex(chat["user_bg"].GetString(), theme.chat.userBg);
    }
  }
  // ─── Syntax palette ──────────────────────────────────────────────────────
  // Same JSON layout as v1's `syntax` block in themes/*.theme.json.
  if (doc.HasMember("syntax") && doc["syntax"].IsObject()) {
    const auto& s = doc["syntax"];
    theme.syntax.keyword = getColor(s, "keyword", theme.syntax.keyword);
    theme.syntax.string = getColor(s, "string", theme.syntax.string);
    theme.syntax.comment = getColor(s, "comment", theme.syntax.comment);
    theme.syntax.number = getColor(s, "number", theme.syntax.number);
    theme.syntax.function = getColor(s, "function", theme.syntax.function);
    theme.syntax.type = getColor(s, "type", theme.syntax.type);
    theme.syntax.op = getColor(s, "op", theme.syntax.op);
    theme.syntax.attr = getColor(s, "attr", theme.syntax.attr);
    theme.syntax.constant = getColor(s, "constant", theme.syntax.constant);
    theme.syntax.variable = getColor(s, "variable", theme.syntax.variable);
    theme.syntax.tag = getColor(s, "tag", theme.syntax.tag);
  }
  // ─── Diff palette (optional) ─────────────────────────────────────────────
  // No v1 theme defines this yet; we keep parsing optional and fall back to
  // the defaults baked into ThemeSpec::Diff.
  if (doc.HasMember("diff") && doc["diff"].IsObject()) {
    const auto& d = doc["diff"];
    theme.diff.addBg = getColor(d, "add_bg", theme.diff.addBg);
    theme.diff.removeBg = getColor(d, "remove_bg", theme.diff.removeBg);
    theme.diff.contextBg = getColor(d, "context_bg", theme.diff.contextBg);
    theme.diff.headerBg = getColor(d, "header_bg", theme.diff.headerBg);
    theme.diff.gutterFg = getColor(d, "gutter_fg", theme.diff.gutterFg);
  }
  return theme;
}

} // namespace

ThemeManager& ThemeManager::instance() {
  static ThemeManager manager;
  return manager;
}

ThemeManager::ThemeManager() { loadThemes(); }

void ThemeManager::loadThemes() {
  themes_.clear();
  const auto dir = PlatformPaths::firmiusHomeDir() / "themes";
  if (std::filesystem::exists(dir) && std::filesystem::is_directory(dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.path().extension() == ".json") {
        themes_.push_back(loadThemeFile(entry.path()));
      }
    }
  }
  if (themes_.empty()) {
    themes_.push_back(defaultTheme());
  }
  loadPersistedSelection();
}

const ThemeSpec& ThemeManager::currentTheme() const {
  return themes_[currentThemeIndex_];
}

std::vector<std::string> ThemeManager::themeNames() const {
  std::vector<std::string> names;
  names.reserve(themes_.size());
  for (const auto& theme : themes_) names.push_back(theme.name);
  return names;
}

bool ThemeManager::setTheme(const std::string& name) {
  for (std::size_t i = 0; i < themes_.size(); ++i) {
    if (themes_[i].name == name) {
      currentThemeIndex_ = i;
      persistSelection();
      return true;
    }
  }
  return false;
}

void ThemeManager::loadPersistedSelection() {
  const auto selected = loadSelectedThemeName();
  if (!selected.has_value()) return;
  for (std::size_t i = 0; i < themes_.size(); ++i) {
    if (themes_[i].name == *selected) {
      currentThemeIndex_ = i;
      return;
    }
  }
}

void ThemeManager::persistSelection() const {
  persistSelectedThemeName(themes_[currentThemeIndex_].name);
}

} // namespace firmius::tui
