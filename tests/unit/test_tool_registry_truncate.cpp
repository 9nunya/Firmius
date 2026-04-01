#include "tools/ToolRegistry.hpp"
#include "agents/Agent.hpp"
#include <iostream>
#include <string>

using namespace firmius::core;
using namespace firmius::shared;

class DummyTool : public ITool {
public:
    ToolMetadata getMetadata() const override {
        return {"dummy", "dummy description", "dummy scope"};
    }
    std::shared_ptr<firmius::shared::ISchema> getSchema() const override {
        // Return a dummy schema that always validates
        class DummySchema : public ISchema {
        public:
            ValidationResult validate(const rapidjson::Value& input) const override {
                return ValidationResult::successResult();
            }
            std::string toString() const override { return "{}"; }
            rapidjson::Document toJSON() const override { return rapidjson::Document(); }
        };
        return std::make_shared<DummySchema>();
    }
    ToolResult execute(const rapidjson::Value& input, ToolContext& ctx) override {
        return ToolResult::ok(std::string(1024 * 1024 + 10, 'A')); // larger than 1MB
    }
};

int main() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<DummyTool>());
    
    // We need an Agent context... which is a bit involved to construct manually, 
    // maybe we can just do:
    // This is a unit test program. Let's try compiling.
}
