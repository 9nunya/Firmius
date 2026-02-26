#include "utils/JSONSchema.hpp"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <algorithm>

namespace firmius::shared {

ValidationResult ValidationResult::ok() { return {true, "", ""}; }
ValidationResult ValidationResult::fail(const std::string& msg, const std::string& p) { return {false, msg, p}; }

std::string ValidationResult::violationToPretty() const {
    if (success) return "Validation passed.";
    std::stringstream ss;
    ss << "Validation Error at " << path << ": " << error;
    return ss.str();
}

std::string JSONSchema::toString() const {
    rapidjson::Document doc;
    doc.SetObject();
    toJson(doc, doc.GetAllocator());
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return buffer.GetString();
}

std::shared_ptr<JSONSchema> JSONSchema::describe(const std::string& desc) {
    description = desc;
    return shared_from_this();
}

bool JSONSchema::isOptional() const { return optional; }

std::shared_ptr<JSONSchema> JSONSchema::setOptional(bool opt) {
    optional = opt;
    return shared_from_this();
}

// StringSchema
ValidationResult StringSchema::validate(const rapidjson::Value& value, const std::string& path) const {
    if (value.IsString()) return ValidationResult::ok();
    // Coercion
    if (value.IsInt() || value.IsUint() || value.IsInt64() || value.IsUint64() || value.IsDouble() || value.IsBool()) {
        return ValidationResult::ok();
    }
    return ValidationResult::fail("Expected string (or coercible type)", path);
}

void StringSchema::toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const {
    output.SetObject();
    output.AddMember("type", "string", allocator);
    if (!description.empty()) {
        output.AddMember("description", rapidjson::Value(description.c_str(), allocator).Move(), allocator);
    }
}

// NumberSchema
NumberSchema::NumberSchema(Mode m) : mode(m) {}

ValidationResult NumberSchema::validate(const rapidjson::Value& value, const std::string& path) const {
    if (mode == Mode::Integer) {
        if (value.IsInt() || value.IsInt64() || value.IsUint() || value.IsUint64()) return ValidationResult::ok();
        if (value.IsString()) {
            try {
                std::stoll(value.GetString());
                return ValidationResult::ok();
            } catch (...) {
                return ValidationResult::fail("Could not coerce string \"" + std::string(value.GetString()) + "\" to integer", path);
            }
        }
        return ValidationResult::fail("Expected integer", path);
    } else {
        if (value.IsNumber()) return ValidationResult::ok();
        if (value.IsString()) {
            try {
                std::stod(value.GetString());
                return ValidationResult::ok();
            } catch (...) {
                return ValidationResult::fail("Could not coerce string \"" + std::string(value.GetString()) + "\" to number", path);
            }
        }
        return ValidationResult::fail("Expected number", path);
    }
}

void NumberSchema::toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const {
    output.SetObject();
    output.AddMember("type", rapidjson::Value(mode == Mode::Integer ? "integer" : "number", allocator).Move(), allocator);
    if (!description.empty()) {
        output.AddMember("description", rapidjson::Value(description.c_str(), allocator).Move(), allocator);
    }
}

// BooleanSchema
ValidationResult BooleanSchema::validate(const rapidjson::Value& value, const std::string& path) const {
    if (value.IsBool()) return ValidationResult::ok();
    if (value.IsString()) {
        std::string s = value.GetString();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
        if (s == "true" || s == "false" || s == "1" || s == "0" || s == "yes" || s == "no") return ValidationResult::ok();
    }
    if (value.IsInt()) {
        if (value.GetInt() == 0 || value.GetInt() == 1) return ValidationResult::ok();
    }
    return ValidationResult::fail("Expected boolean (or coercible type)", path);
}

void BooleanSchema::toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const {
    output.SetObject();
    output.AddMember("type", "boolean", allocator);
    if (!description.empty()) {
        output.AddMember("description", rapidjson::Value(description.c_str(), allocator).Move(), allocator);
    }
}

// ObjectSchema
std::shared_ptr<ObjectSchema> ObjectSchema::property(const std::string& name, std::shared_ptr<JSONSchema> schema) {
    properties[name] = schema;
    return std::static_pointer_cast<ObjectSchema>(shared_from_this());
}

std::shared_ptr<ObjectSchema> ObjectSchema::required(const std::vector<std::string>& req) {
    requiredFields = req;
    return std::static_pointer_cast<ObjectSchema>(shared_from_this());
}

ValidationResult ObjectSchema::validate(const rapidjson::Value& value, const std::string& path) const {
    if (!value.IsObject()) return ValidationResult::fail("Expected object", path);

    for (const auto& req : requiredFields) {
        if (!value.HasMember(req.c_str()) || value[req.c_str()].IsNull()) {
            return ValidationResult::fail("Missing required field: " + req, path);
        }
    }

    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        std::string name = it->name.GetString();
        auto propIt = properties.find(name);
        if (propIt != properties.end()) {
            auto res = propIt->second->validate(it->value, path + "." + name);
            if (!res.success) return res;
        }
    }

    return ValidationResult::ok();
}

void ObjectSchema::toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const {
    output.SetObject();
    output.AddMember("type", "object", allocator);
    if (!description.empty()) {
        output.AddMember("description", rapidjson::Value(description.c_str(), allocator).Move(), allocator);
    }

    rapidjson::Value props(rapidjson::kObjectType);
    for (const auto& [name, schema] : properties) {
        rapidjson::Value propSchema(rapidjson::kObjectType);
        schema->toJson(propSchema, allocator);
        props.AddMember(rapidjson::Value(name.c_str(), allocator).Move(), propSchema, allocator);
    }
    output.AddMember("properties", props, allocator);

    if (!requiredFields.empty()) {
        rapidjson::Value reqArr(rapidjson::kArrayType);
        for (const auto& r : requiredFields) {
            reqArr.PushBack(rapidjson::Value(r.c_str(), allocator).Move(), allocator);
        }
        output.AddMember("required", reqArr, allocator);
    }
}

// ArraySchema
ArraySchema::ArraySchema(std::shared_ptr<JSONSchema> items) : itemSchema(items) {}

ValidationResult ArraySchema::validate(const rapidjson::Value& value, const std::string& path) const {
    if (!value.IsArray()) return ValidationResult::fail("Expected array", path);

    for (rapidjson::SizeType i = 0; i < value.Size(); ++i) {
        auto res = itemSchema->validate(value[i], path + "[" + std::to_string(i) + "]");
        if (!res.success) return res;
    }

    return ValidationResult::ok();
}

void ArraySchema::toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const {
    output.SetObject();
    output.AddMember("type", "array", allocator);
    if (!description.empty()) {
        output.AddMember("description", rapidjson::Value(description.c_str(), allocator).Move(), allocator);
    }

    rapidjson::Value items(rapidjson::kObjectType);
    itemSchema->toJson(items, allocator);
    output.AddMember("items", items, allocator);
}

// Factory methods
std::shared_ptr<StringSchema> zString() { return std::make_shared<StringSchema>(); }
std::shared_ptr<NumberSchema> zNumber() { return std::make_shared<NumberSchema>(NumberSchema::Mode::Float); }
std::shared_ptr<NumberSchema> zInteger() { return std::make_shared<NumberSchema>(NumberSchema::Mode::Integer); }
std::shared_ptr<BooleanSchema> zBoolean() { return std::make_shared<BooleanSchema>(); }
std::shared_ptr<ObjectSchema> zObject(const std::map<std::string, std::shared_ptr<JSONSchema>>& props) {
    auto obj = std::make_shared<ObjectSchema>();
    for (const auto& [name, schema] : props) {
        obj->property(name, schema);
    }
    return obj;
}
std::shared_ptr<ArraySchema> zArray(std::shared_ptr<JSONSchema> items) { return std::make_shared<ArraySchema>(items); }

}
