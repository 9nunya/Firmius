#include <gtest/gtest.h>
#include "utils/JSONSchema.hpp"
#include <rapidjson/document.h>
#include <iostream>

using namespace firmius::shared;

TEST(JSONSchema, CoercionAndBreadcrumbs) {
    auto schema = zObject({
        {"count", zInteger()->describe("A count")},
        {"active", zBoolean()},
        {"tags", zArray(zString())}
    })->required({"count", "active"});

    rapidjson::Document doc;
    doc.SetObject();
    auto& a = doc.GetAllocator();
    doc.AddMember("count", "123", a); // Coercible string
    doc.AddMember("active", "true", a); // Coercible string
    
    rapidjson::Value tags(rapidjson::kArrayType);
    tags.PushBack(1, a); // Coercible int to string
    tags.PushBack(true, a); // Coercible bool to string
    doc.AddMember("tags", tags, a);

    auto res = schema->validate(doc);
    EXPECT_TRUE(res.success) << res.violationToPretty();

    // Violation test
    rapidjson::Document badDoc;
    badDoc.SetObject();
    badDoc.AddMember("count", "abc", a); // Non-coercible
    badDoc.AddMember("active", true, a); // Add field to satisfy required but count is wrong
    
    auto res2 = schema->validate(badDoc);
    std::cout << "DEBUG: " << res2.violationToPretty() << std::endl;
    EXPECT_FALSE(res2.success);
    EXPECT_TRUE(res2.violationToPretty().find("root.count") != std::string::npos);
    EXPECT_TRUE(res2.violationToPretty().find("Could not coerce") != std::string::npos);
}

TEST(JSONSchema, NestedBreadcrumbs) {
    auto schema = zObject({
        {"users", zArray(zObject({
            {"id", zInteger()},
            {"name", zString()}
        }))}
    });

    rapidjson::Document doc;
    doc.SetObject();
    auto& a = doc.GetAllocator();
    
    rapidjson::Value users(rapidjson::kArrayType);
    rapidjson::Value u1(rapidjson::kObjectType);
    u1.AddMember("id", 1, a);
    u1.AddMember("name", "Alice", a);
    users.PushBack(u1, a);
    
    rapidjson::Value u2(rapidjson::kObjectType);
    u2.AddMember("id", "not-an-int", a);
    u2.AddMember("name", "Bob", a);
    users.PushBack(u2, a);
    
    doc.AddMember("users", users, a);

    auto res = schema->validate(doc);
    EXPECT_FALSE(res.success);
    EXPECT_TRUE(res.violationToPretty().find("root.users[1].id") != std::string::npos);
}

#include "Panic.hpp"
TEST(Panic, Backtrace) {
    firmius::shared::Panic::init();
    // Deliberate panic to verify backtrace symbolication
    // FIRMIUS_PANIC("Test Panic for Backtrace");
}
