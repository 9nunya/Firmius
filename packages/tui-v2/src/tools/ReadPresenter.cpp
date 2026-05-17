#include "tools/ReadPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>

namespace firmius::tui2 {

bool ReadPresenter::matches(const std::string& toolName) const {
  return toolName == "Read";
}

namespace {

struct ReadArgs {
  std::string path;
  int startLine = 1;
  int endLine = -1;
  bool hasStartLine = false;
  bool hasEndLine = false;
};

ReadArgs parseArgs(const std::string& json) {
  ReadArgs a;
  if (json.empty()) return a;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return a;
  if (doc.HasMember("path") && doc["path"].IsString()) a.path = doc["path"].GetString();
  if (doc.HasMember("start_line") && doc["start_line"].IsInt()) { a.startLine = doc["start_line"].GetInt(); a.hasStartLine = true; }
  if (doc.HasMember("end_line") && doc["end_line"].IsInt()) { a.endLine = doc["end_line"].GetInt(); a.hasEndLine = true; }
  return a;
}

int parseLinesRead(const std::string& json) {
  if (json.empty()) return 0;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return 0;
  if (doc.HasMember("lines_read") && doc["lines_read"].IsInt()) return doc["lines_read"].GetInt();
  return 0;
}

} // namespace

std::vector<std::string> ReadPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Read")};
  }

  auto args = parseArgs(item.args());

  if (item.phase() == ToolPhase::Called) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Reading " + args.path + "...")};
  }

  // Finished
  int linesRead = parseLinesRead(item.result());
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";

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
  if (linesRead > 0) {
    text += " (" + std::to_string(linesRead) + " lines)";
  }
  return {success ? theme_ansi::success(text) : theme_ansi::error(text)};
}

} // namespace firmius::tui2
