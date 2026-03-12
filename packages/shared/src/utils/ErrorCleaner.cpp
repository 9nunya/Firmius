#include "utils/ErrorCleaner.hpp"
#include <rapidjson/document.h>

namespace firmius::shared {

std::string ErrorCleaner::clean(const std::string &error) {
  if (error.empty())
    return "unknown error";

  // Strip "Invalid JSON arguments: " prefix
  static const std::string json_prefix = "Invalid JSON arguments: ";
  if (error.find(json_prefix) == 0) {
    std::string json_part = error.substr(json_prefix.size());

    // Try to find the actual error message if it's a specific pattern
    // e.g. "Invalid JSON arguments: {"persona":"Missing required property"}"
    rapidjson::Document doc;
    doc.Parse(json_part.c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      std::string cleaned;
      for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        if (!cleaned.empty())
          cleaned += ", ";
        cleaned += std::string(it->name.GetString()) + ": ";
        if (it->value.IsString()) {
          cleaned += it->value.GetString();
        } else {
          cleaned += "invalid format";
        }
      }
      if (!cleaned.empty())
        return cleaned;
    }
    return json_part;
  }

  // Strip generic "Error: " prefix
  static const std::string err_prefix = "Error: ";
  if (error.find(err_prefix) == 0) {
    return error.substr(err_prefix.size());
  }

  return error;
}

} // namespace firmius::shared
