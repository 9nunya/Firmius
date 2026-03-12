#include "utils/ErrorCleaner.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <vector>

namespace firmius::shared {

namespace {

std::string valueToString(const rapidjson::Value &v) {
  if (v.IsString())
    return v.GetString();
  if (v.IsInt())
    return std::to_string(v.GetInt());
  if (v.IsUint())
    return std::to_string(v.GetUint());
  if (v.IsInt64())
    return std::to_string(v.GetInt64());
  if (v.IsUint64())
    return std::to_string(v.GetUint64());
  if (v.IsDouble())
    return std::to_string(v.GetDouble());
  if (v.IsBool())
    return v.GetBool() ? "true" : "false";
  if (v.IsNull())
    return "null";

  if (v.IsObject() || v.IsArray()) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    v.Accept(writer);
    return buffer.GetString();
  }
  return "unknown";
}

std::string extractFromObject(const rapidjson::Value &obj) {
  if (!obj.IsObject())
    return valueToString(obj);

  // Common error fields in order of preference
  const char *fields[] = {"message", "error",       "msg",
                          "detail",  "description", "status"};
  for (const char *field : fields) {
    if (obj.HasMember(field)) {
      if (obj[field].IsString())
        return obj[field].GetString();
      if (obj[field].IsObject())
        return extractFromObject(obj[field]);
    }
  }

  // Fallback: collect all members
  std::string collected;
  for (auto it = obj.MemberBegin(); it != obj.MemberEnd(); ++it) {
    if (!collected.empty())
      collected += ", ";
    collected +=
        std::string(it->name.GetString()) + ": " + valueToString(it->value);
  }
  return collected;
}

} // namespace

std::string ErrorCleaner::clean(const std::string &error) {
  if (error.empty())
    return "unknown error";

  std::string to_parse = error;

  // Strip common prefixes
  static const std::vector<std::string> prefixes = {
      "Invalid JSON arguments: ", "Error: ", "Exception: ", "Runtime Error: "};

  for (const auto &prefix : prefixes) {
    if (to_parse.find(prefix) == 0) {
      to_parse = to_parse.substr(prefix.size());
      break;
    }
  }

  // Try parsing as JSON
  rapidjson::Document doc;
  doc.Parse(to_parse.c_str());
  if (!doc.HasParseError()) {
    if (doc.IsObject()) {
      std::string extracted = extractFromObject(doc);
      if (!extracted.empty())
        return extracted;
    } else if (doc.IsString()) {
      return doc.GetString();
    }
  }

  // Fallback: If it still looks like JSON but couldn't be parsed/extracted
  // cleanly, return the stripped version.
  return to_parse;
}

} // namespace firmius::shared
