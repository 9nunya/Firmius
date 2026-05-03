#ifndef FIRMIUS_SHARED_JSON_SCHEMA_FROM_JSON_HPP
#define FIRMIUS_SHARED_JSON_SCHEMA_FROM_JSON_HPP

#include "utils/JSONSchema.hpp"

#include <rapidjson/document.h>
#include <memory>
#include <string>

namespace firmius::shared {

/// Parse a JSON Schema (draft-07-ish subset) from a RapidJSON value.
/// Supported subset:
///   - {"type":"object","properties":{...},"required":[...]}
///   - {"type":"array","items":...}
///   - {"type":"string"|"number"|"integer"|"boolean"}
///   - {"enum":["a","b",...]}
///
/// Unknown/unsupported schema shapes fall back to permissive `zObject()`.
std::shared_ptr<JSONSchema> jsonSchemaFromJson(const rapidjson::Value &schema);

/// Parse from a JSON string. Returns nullptr on parse errors.
std::shared_ptr<JSONSchema> jsonSchemaFromString(const std::string &schemaJson);

} // namespace firmius::shared

#endif