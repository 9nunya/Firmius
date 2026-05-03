#include "utils/JSONSchemaFromJson.hpp"

#include <rapidjson/document.h>

#include <memory>
#include <string>
#include <vector>

namespace firmius::shared {

namespace {

std::shared_ptr<JSONSchema> fromTypeString(const std::string &t) {
  if (t == "string") return zString();
  if (t == "number") return zNumber();
  if (t == "integer") return zInteger();
  if (t == "boolean") return zBoolean();
  if (t == "object") return zObject();
  if (t == "array") return zArray(zObject());
  return zObject();
}

} // namespace

std::shared_ptr<JSONSchema> jsonSchemaFromJson(const rapidjson::Value &schema) {
  if (!schema.IsObject()) {
    return zObject();
  }

  // enum short-circuit
  if (schema.HasMember("enum") && schema["enum"].IsArray()) {
    std::vector<std::string> values;
    for (const auto &v : schema["enum"].GetArray()) {
      if (v.IsString()) values.emplace_back(v.GetString());
    }
    if (!values.empty()) return zEnum(values);
  }

  std::string type;
  if (schema.HasMember("type") && schema["type"].IsString()) {
    type = schema["type"].GetString();
  }

  if (type == "object") {
    auto obj = zObject();
    if (schema.HasMember("properties") && schema["properties"].IsObject()) {
      for (auto it = schema["properties"].MemberBegin();
           it != schema["properties"].MemberEnd(); ++it) {
        if (!it->name.IsString()) continue;
        obj->property(it->name.GetString(), jsonSchemaFromJson(it->value));
      }
    }
    if (schema.HasMember("required") && schema["required"].IsArray()) {
      std::vector<std::string> req;
      for (const auto &r : schema["required"].GetArray()) {
        if (r.IsString()) req.emplace_back(r.GetString());
      }
      if (!req.empty()) obj->required(req);
    }
    return obj;
  }

  if (type == "array") {
    std::shared_ptr<JSONSchema> items = zObject();
    if (schema.HasMember("items")) {
      items = jsonSchemaFromJson(schema["items"]);
    }
    return zArray(items);
  }

  if (!type.empty()) {
    return fromTypeString(type);
  }

  // Fallback: treat schema as permissive object.
  return zObject();
}

std::shared_ptr<JSONSchema> jsonSchemaFromString(const std::string &schemaJson) {
  if (schemaJson.empty()) return nullptr;
  rapidjson::Document d;
  if (d.Parse(schemaJson.c_str()).HasParseError()) return nullptr;
  return jsonSchemaFromJson(d);
}

} // namespace firmius::shared
