#include "items/QuickToolClusterItem.hpp"

#include "Terminal.hpp"
#include "ThemeAnsi.hpp"
#include "ThemeManager.hpp"

#include <rapidjson/document.h>

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace firmius::tui {

namespace {

bool isQuickTool(const std::string& name) {
  return name == "Read" || name == "Grep" || name == "Glob" || name == "List";
}

std::string jsonString(const rapidjson::Document& doc, const char* field) {
  if (doc.HasMember(field) && doc[field].IsString()) return doc[field].GetString();
  return "";
}

int jsonInt(const rapidjson::Document& doc, const char* field, int def = 0) {
  if (doc.HasMember(field) && doc[field].IsInt()) return doc[field].GetInt();
  return def;
}

std::string labelFor(const std::string& toolName, const std::string& argsJson) {
  rapidjson::Document doc;
  if (!argsJson.empty()) doc.Parse(argsJson.c_str());

  if (toolName == "Read") {
    if (doc.HasParseError() || !doc.IsObject()) return "";
    const std::string path = jsonString(doc, "path");
    const bool hasStart = doc.HasMember("start_line") && doc["start_line"].IsInt();
    const bool hasEnd = doc.HasMember("end_line") && doc["end_line"].IsInt();
    if (!hasStart && !hasEnd) return path;
    const int start = jsonInt(doc, "start_line", 1);
    const int end = jsonInt(doc, "end_line", -1);
    std::string range = std::to_string(start) + "-";
    if (hasEnd && end >= 0) range += std::to_string(end);
    return path + ":" + range;
  }

  if (toolName == "List") {
    if (doc.HasParseError() || !doc.IsObject()) return "";
    return jsonString(doc, "path");
  }

  if (toolName == "Grep") {
    if (doc.HasParseError() || !doc.IsObject()) return "";
    const std::string path = jsonString(doc, "path");
    const std::string pat = jsonString(doc, "pattern");
    if (pat.empty()) return path;
    if (path.empty()) return "\"" + pat + "\"";
    return "\"" + pat + "\" in " + path;
  }

  if (toolName == "Glob") {
    if (doc.HasParseError() || !doc.IsObject()) return "";
    const std::string path = jsonString(doc, "path");
    std::string pat = jsonString(doc, "glob");
    if (pat.empty()) pat = jsonString(doc, "pattern");
    if (pat.empty()) return path;
    if (path.empty()) return "\"" + pat + "\"";
    return "\"" + pat + "\" in " + path;
  }

  return "";
}

std::string presentParticiple(const std::string& toolName) {
  if (toolName == "Read") return "Reading";
  if (toolName == "List") return "Listing";
  if (toolName == "Grep") return "Grepping";
  if (toolName == "Glob") return "Globbing";
  return "Working";
}

std::string pastTense(const std::string& toolName) {
  if (toolName == "Read") return "Read";
  if (toolName == "List") return "Listed";
  if (toolName == "Grep") return "Grepped";
  if (toolName == "Glob") return "Globbed";
  return "Worked";
}

ThemeRgb toolColor(const std::string& toolName) {
  const auto& theme = ThemeManager::instance().currentTheme();
  if (toolName == "Read") return theme.base.highlight;
  if (toolName == "List") return theme.statusBar.context.low;
  if (toolName == "Grep") return theme.statusBar.context.medium;
  if (toolName == "Glob") return theme.statusBar.context.high;
  return theme.base.fg;
}

std::string toolVerb(const std::string& toolName, bool inFlight) {
  if (toolName == "Read") return inFlight ? "Read " : "Read ";
  if (toolName == "List") return inFlight ? "Listed " : "Listed ";
  if (toolName == "Grep") return inFlight ? "Grep " : "Grep ";
  if (toolName == "Glob") return inFlight ? "Glob " : "Glob ";
  return toolName + " ";
}

std::vector<std::string> wrapCommaList(const std::vector<std::string>& items, int maxWidth) {
  std::vector<std::string> lines;
  std::string cur;
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string piece = (cur.empty() ? "" : ", ") + items[i];
    if (!cur.empty() && ansi::visibleWidth(cur + piece) > maxWidth) {
      lines.push_back(cur);
      cur = items[i];
    } else {
      cur += piece;
    }
  }
  if (!cur.empty()) lines.push_back(cur);
  if (lines.empty()) lines.push_back("");
  return lines;
}

} // namespace

void QuickToolClusterItem::addOrUpdateCall(const std::string& toolCallId,
                                          const std::string& toolName,
                                          const std::string& args,
                                          bool inFlight) {
  if (!isQuickTool(toolName) || toolCallId.empty()) return;
  for (auto& e : entries_) {
    if (e.toolCallId == toolCallId) {
      e.toolName = toolName;
      e.args = args;
      e.label = labelFor(toolName, args);
      e.inFlight = inFlight;
      touch();
      return;
    }
  }
  Entry e;
  e.toolCallId = toolCallId;
  e.toolName = toolName;
  e.args = args;
  e.label = labelFor(toolName, args);
  e.inFlight = inFlight;
  entries_.push_back(std::move(e));
  touch();
}

void QuickToolClusterItem::setResult(const std::string& toolCallId, bool success, const std::string& /*result*/) {
  for (auto& e : entries_) {
    if (e.toolCallId == toolCallId) {
      e.inFlight = false;
      e.hasResult = true;
      e.success = success;
      touch();
      return;
    }
  }
}

void QuickToolClusterItem::finalize() {
  finalized_ = true;
  for (auto& e : entries_) e.inFlight = false;
  touch();
}

std::vector<std::string> QuickToolClusterItem::render(int width) const {
  constexpr int kIndent = 2;
  const int innerWidth = std::max(1, width - kIndent);

  bool anyInFlight = false;
  std::vector<std::string> kindsOrdered;
  std::unordered_map<std::string, bool> seen;
  for (const auto& e : entries_) {
    anyInFlight = anyInFlight || e.inFlight;
    if (!seen[e.toolName]) {
      seen[e.toolName] = true;
      kindsOrdered.push_back(e.toolName);
    }
  }

  std::string header = "Quick tools";
  if (!kindsOrdered.empty()) {
    std::vector<std::string> words;
    for (const auto& k : kindsOrdered) {
      words.push_back(anyInFlight ? presentParticiple(k) : pastTense(k));
    }
    header = words.front();
    for (size_t i = 1; i < words.size(); ++i) {
      header += (i + 1 == words.size()) ? " and " : ", ";
      header += words[i];
    }
    header += anyInFlight ? ".." : ".";
  }

  std::vector<std::string> out;
  out.push_back(std::string(kIndent, ' ') + theme_ansi::foreground(header));

  // Group entries by tool kind in appearance order.
  std::vector<std::string> groupOrder;
  std::unordered_map<std::string, std::vector<std::string>> grouped;
  for (const auto& e : entries_) {
    if (!seen.count(e.toolName)) continue;
    if (grouped.find(e.toolName) == grouped.end()) groupOrder.push_back(e.toolName);
    std::string label = e.label;
    if (label.empty()) label = e.toolName;
    grouped[e.toolName].push_back(label);
  }

  for (const auto& kind : groupOrder) {
    const auto& items = grouped[kind];
    if (items.empty()) continue;
    const std::string lead = toolVerb(kind, anyInFlight);
    const auto color = toolColor(kind);
    auto wrapped = wrapCommaList(items, std::max(1, innerWidth - 3 - static_cast<int>(lead.size())));
    for (size_t i = 0; i < wrapped.size(); ++i) {
      const bool last = (kind == groupOrder.back()) && (i + 1 == wrapped.size());
      const std::string branch = last ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x82  "; // └─ / │
      std::string content;
      if (i == 0) {
        content = ansi::fgRgb(color.r, color.g, color.b, lead) +
                  theme_ansi::foreground(wrapped[i]);
      } else {
        content =
            std::string(static_cast<std::size_t>(ansi::visibleWidth(lead)), ' ') +
            theme_ansi::foreground(wrapped[i]);
      }
      out.push_back(std::string(kIndent, ' ') + theme_ansi::dim(branch) + content);
    }
  }
  return out;
}

int QuickToolClusterItem::rowCount(int width) const {
  return static_cast<int>(render(width).size());
}

} // namespace firmius::tui
