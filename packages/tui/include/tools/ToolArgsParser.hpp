#ifndef FIRMIUS_TUI_TOOLARGSPARSER_HPP
#define FIRMIUS_TUI_TOOLARGSPARSER_HPP

#include <rapidjson/document.h>
#include <string>

namespace firmius::tui::toolArgsParser {

/// Extract the "action" field from a JSON args string.
/// Returns empty string if not found or not a string.
inline std::string extractAction(const std::string& argsJson) {
  if (argsJson.empty()) return "";
  rapidjson::Document doc;
  doc.Parse(argsJson.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return "";
  if (doc.HasMember("action") && doc["action"].IsString()) {
    return doc["action"].GetString();
  }
  return "";
}

} // namespace firmius::tui::toolArgsParser

#endif // FIRMIUS_TUI_TOOLARGSPARSER_HPP
