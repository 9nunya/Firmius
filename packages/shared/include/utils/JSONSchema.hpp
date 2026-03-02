#ifndef FIRMIUS_CORE_JSON_SCHEMA_HPP
#define FIRMIUS_CORE_JSON_SCHEMA_HPP

#include <rapidjson/document.h>
#include <string>
#include <vector>
#include <memory>
#include <map>

/**
 * @brief Zod-inspired JSON Schema validation and building.
 */
namespace firmius::shared {

/**
 * @brief Result of a JSON schema validation.
 */
struct ValidationResult {
    bool success;       ///< True if validation passed.
    std::string error;  ///< Error message on failure.
    std::string path;   ///< Breadcrumb path to the violation.

    /**
     * @brief Creates a successful validation result.
     */
    static ValidationResult ok();

    /**
     * @brief Creates a failed validation result.
     * @param msg Descriptive error message.
     * @param p Breadcrumb path.
     */
    static ValidationResult fail(const std::string& msg, const std::string& p = "");

    /**
     * @brief Formats the violation into a human-readable string.
     */
    std::string violationToPretty() const;
};

/**
 * @brief Base class for all JSON schema types.
 */
class JSONSchema : public std::enable_shared_from_this<JSONSchema> {
public:
    virtual ~JSONSchema() = default;

    /**
     * @brief Validates a JSON value against the schema.
     * @param value The value to validate.
     * @param path Current breadcrumb path.
     */
    virtual ValidationResult validate(const rapidjson::Value& value, const std::string& path = "root") const = 0;

    /**
     * @brief Serializes the schema to a JSON representation.
     */
    virtual void toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const = 0;

    /**
     * @brief Convenience function to get schema as a JSON string.
     */
    std::string toString() const;

    /**
     * @brief Adds a description to the schema for LLM guidance.
     */
    std::shared_ptr<JSONSchema> describe(const std::string& desc);

    /**
     * @brief Returns true if this field is optional.
     */
    bool isOptional() const;

    /**
     * @brief Marks this schema as optional.
     */
    std::shared_ptr<JSONSchema> setOptional(bool opt = true);

protected:
    std::string description;
    bool optional = false;
};

/**
 * @brief Schema for string types. Supports coercion from other primitives.
 */
class StringSchema : public JSONSchema {
public:
    ValidationResult validate(const rapidjson::Value& value, const std::string& path = "root") const override;
    void toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const override;
};

/**
 * @brief Schema for numeric types. Supports coercion from strings.
 */
class NumberSchema : public JSONSchema {
public:
    enum class Mode { Float, Integer };
    NumberSchema(Mode m);

    ValidationResult validate(const rapidjson::Value& value, const std::string& path = "root") const override;
    void toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const override;

private:
    Mode mode;
};

/**
 * @brief Schema for boolean types. Supports coercion from truthy strings/ints.
 */
class BooleanSchema : public JSONSchema {
public:
    ValidationResult validate(const rapidjson::Value& value, const std::string& path = "root") const override;
    void toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const override;
};

/**
 * @brief Schema for object types with nested properties.
 */
class ObjectSchema : public JSONSchema {
public:
    /**
     * @brief Adds a property to the object.
     */
    std::shared_ptr<ObjectSchema> property(const std::string& name, std::shared_ptr<JSONSchema> schema);

    /**
     * @brief Sets required fields.
     */
    std::shared_ptr<ObjectSchema> required(const std::vector<std::string>& req);

    ValidationResult validate(const rapidjson::Value& value, const std::string& path = "root") const override;
    void toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const override;

private:
    std::map<std::string, std::shared_ptr<JSONSchema>> properties;
    std::vector<std::string> requiredFields;
};

/**
 * @brief Schema for array types.
 */
class ArraySchema : public JSONSchema {
public:
    ArraySchema(std::shared_ptr<JSONSchema> items);

    ValidationResult validate(const rapidjson::Value& value, const std::string& path = "root") const override;
    void toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const override;

private:
    std::shared_ptr<JSONSchema> itemSchema;
};

/**
 * @brief Schema for string enums. Validates that value is one of a fixed set of strings.
 */
class EnumSchema : public JSONSchema {
public:
    explicit EnumSchema(std::vector<std::string> values);

    ValidationResult validate(const rapidjson::Value& value, const std::string& path = "root") const override;
    void toJson(rapidjson::Value& output, rapidjson::Document::AllocatorType& allocator) const override;

private:
    std::vector<std::string> allowedValues;
};

// Factory methods
std::shared_ptr<StringSchema> zString();
std::shared_ptr<NumberSchema> zNumber();
std::shared_ptr<NumberSchema> zInteger();
std::shared_ptr<BooleanSchema> zBoolean();
std::shared_ptr<EnumSchema> zEnum(const std::vector<std::string>& values);
std::shared_ptr<ObjectSchema> zObject(const std::map<std::string, std::shared_ptr<JSONSchema>>& props = {});
std::shared_ptr<ArraySchema> zArray(std::shared_ptr<JSONSchema> items);

}

#endif
