#include "tools/FilesPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"

#include <rapidjson/document.h>

namespace firmius::tui2 {

bool FilesPresenter::matches(const std::string& toolName) const {
  return toolName == "Files";
}

namespace {

struct FilesArgs {
  std::string action;
  std::string path;
  int startLine = 1;
  int endLine = -1;
  bool hasStartLine = false;
  bool hasEndLine = false;
};

FilesArgs parseArgs(const std::string& json) {
  FilesArgs a;
  if (json.empty()) return a;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return a;
  if (doc.HasMember("action") && doc["action"].IsString()) a.action = doc["action"].GetString();
  if (doc.HasMember("path") && doc["path"].IsString()) a.path = doc["path"].GetString();
  if (doc.HasMember("start_line") && doc["start_line"].IsInt()) { a.startLine = doc["start_line"].GetInt(); a.hasStartLine = true; }
  if (doc.HasMember("end_line") && doc["end_line"].IsInt()) { a.endLine = doc["end_line"].GetInt(); a.hasEndLine = true; }
  return a;
}

struct FilesResult {
  int linesRead = 0;
  int entryCount = 0;
  bool budgetHit = false;
};

FilesResult parseResult(const std::string& json, const std::string& action) {
  FilesResult r;
  if (json.empty()) return r;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return r;

  // List returns a JSON array directly
  if (action == "List" && doc.IsArray()) {
    r.entryCount = static_cast<int>(doc.Size());
    return r;
  }

  if (!doc.IsObject()) return r;
  if (doc.HasMember("lines_read") && doc["lines_read"].IsInt()) r.linesRead = doc["lines_read"].GetInt();
  if (doc.HasMember("budget_hit") && doc["budget_hit"].IsBool()) r.budgetHit = doc["budget_hit"].GetBool();

  // Grep/Glob return {results: [...], budget_hit: ...}
  if (doc.HasMember("results") && doc["results"].IsArray()) {
    r.entryCount = static_cast<int>(doc["results"].Size());
  }
  // Fallback: count field (legacy)
  if (r.entryCount == 0 && doc.HasMember("count") && doc["count"].IsInt()) {
    r.entryCount = doc["count"].GetInt();
  }
  return r;
}

} // namespace

std::vector<std::string> FilesPresenter::render(const ToolCallItem& item, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Files")};
  }

  auto args = parseArgs(item.args());

  if (item.phase() == ToolPhase::Called) {
    if (args.action == "Read") {
      return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Reading " + args.path + "...")};
    }
    if (args.action == "List") {
      return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Listing " + args.path + "...")};
    }
    if (args.action == "Grep") {
      std::string pattern;
      rapidjson::Document doc;
      doc.Parse(item.args().c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("pattern") && doc["pattern"].IsString()) {
        pattern = doc["pattern"].GetString();
      }
      return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Grep \"" + pattern + "\" in " + args.path + "...")};
    }
    if (args.action == "Glob") {
      std::string glob;
      rapidjson::Document doc;
      doc.Parse(item.args().c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("glob") && doc["glob"].IsString()) glob = doc["glob"].GetString();
        else if (doc.HasMember("pattern") && doc["pattern"].IsString()) glob = doc["pattern"].GetString();
      }
      return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Glob \"" + glob + "\" in " + args.path + "...")};
    }
    return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 " + args.action + " " + args.path)};
  }

  // Finished
  auto res = parseResult(item.result(), args.action);
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";

  if (args.action == "Read") {
    std::string text = "  " + icon + " Read " + args.path;
    if (args.hasStartLine || args.hasEndLine) {
      std::string range;
      if (args.hasStartLine) range += std::to_string(args.startLine);
      range += "-";
      if (args.hasEndLine) range += std::to_string(args.endLine);
      text += " \xe2\x80\x94 lines " + range;
    } else {
      text += " \xe2\x80\x94 full file";
    }
    if (res.linesRead > 0) {
      text += " (" + std::to_string(res.linesRead) + " lines)";
    }
    return {success ? ansi::fgRgb(100, 200, 120, text) : ansi::fgRgb(220, 80, 80, text)};
  }

  if (args.action == "List") {
    std::string text = "  " + icon + " Listed " + args.path + " \xe2\x80\x94 " +
                       std::to_string(res.entryCount) + " entries";
    return {success ? ansi::fgRgb(100, 200, 120, text) : ansi::fgRgb(220, 80, 80, text)};
  }

  if (args.action == "Grep") {
    std::string pattern;
    rapidjson::Document d;
    d.Parse(item.args().c_str());
    if (!d.HasParseError() && d.IsObject() && d.HasMember("pattern") && d["pattern"].IsString())
      pattern = d["pattern"].GetString();
    std::string text = "  " + icon + " Grep";
    if (!pattern.empty()) text += " \"" + pattern + "\"";
    if (!args.path.empty()) text += " in " + args.path;
    text += " \xe2\x80\x94 " + std::to_string(res.entryCount) + " matches";
    if (res.budgetHit) text += " \xe2\x80\x94 budget hit";
    return {success ? ansi::fgRgb(100, 200, 120, text) : ansi::fgRgb(220, 80, 80, text)};
  }

  if (args.action == "Glob") {
    std::string glob;
    rapidjson::Document d;
    d.Parse(item.args().c_str());
    if (!d.HasParseError() && d.IsObject()) {
      if (d.HasMember("glob") && d["glob"].IsString()) glob = d["glob"].GetString();
      else if (d.HasMember("pattern") && d["pattern"].IsString()) glob = d["pattern"].GetString();
    }
    std::string text = "  " + icon + " Glob";
    if (!glob.empty()) text += " \"" + glob + "\"";
    if (!args.path.empty()) text += " in " + args.path;
    text += " \xe2\x80\x94 " + std::to_string(res.entryCount) + " files";
    if (res.budgetHit) text += " \xe2\x80\x94 budget hit";
    return {success ? ansi::fgRgb(100, 200, 120, text) : ansi::fgRgb(220, 80, 80, text)};
  }

  std::string text = "  " + icon + " " + args.action;
  if (!args.path.empty()) text += " " + args.path;
  return {success ? ansi::fgRgb(100, 200, 120, text)
                  : ansi::fgRgb(220, 80, 80, text)};
}

} // namespace firmius::tui2
