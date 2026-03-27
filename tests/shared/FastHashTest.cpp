#include <gtest/gtest.h>
#include "utils/FastHash.hpp"
#include <string>

using namespace firmius::shared::utils;

TEST(FastHashTest, BasicOperations) {
    FastHash<std::string, int> map;
    
    map.set("one", 1);
    map.set("two", 2);
    
    EXPECT_EQ(map.size(), 2);
    EXPECT_TRUE(map.contains("one"));
    EXPECT_TRUE(map.contains("two"));
    EXPECT_FALSE(map.contains("three"));
    
    EXPECT_EQ(*map.get("one"), 1);
    EXPECT_EQ(*map.get("two"), 2);
    EXPECT_EQ(map.get("three"), nullptr);
}

TEST(FastHashTest, OperatorBracket) {
    FastHash<std::string, std::string> map;
    
    map["hello"] = "world";
    EXPECT_EQ(map["hello"], "world");
    EXPECT_EQ(map.size(), 1);
    
    // Test default insertion
    std::string& val = map["new"];
    EXPECT_EQ(val, "");
    EXPECT_EQ(map.size(), 2);
}

TEST(FastHashTest, Erase) {
    FastHash<std::string, int> map;
    map.set("a", 1);
    map.set("b", 2);
    
    map.erase("a");
    EXPECT_EQ(map.size(), 1);
    EXPECT_FALSE(map.contains("a"));
    EXPECT_TRUE(map.contains("b"));
    
    map.erase("nonexistent");
    EXPECT_EQ(map.size(), 1);
}

TEST(FastHashTest, Resize) {
    FastHash<std::string, int> map(4); // Small initial capacity
    
    for (int i = 0; i < 100; ++i) {
        map.set("key" + std::to_string(i), i);
    }
    
    EXPECT_EQ(map.size(), 100);
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(map.contains("key" + std::to_string(i)));
        EXPECT_EQ(*map.get("key" + std::to_string(i)), i);
    }
}

TEST(FastHashTest, Iterator) {
    FastHash<std::string, int> map;
    map.set("one", 1);
    map.set("two", 2);
    map.set("three", 3);
    
    int sum = 0;
    int count = 0;
    for (auto it = map.begin(); it != map.end(); ++it) {
        sum += it->second;
        count++;
    }
    
    EXPECT_EQ(sum, 6);
    EXPECT_EQ(count, 3);
}

TEST(FastHashTest, RobinHoodCollision) {
    FastHash<std::string, int> map(16);
    
    for (int i = 0; i < 200; ++i) {
        map.set("item_" + std::to_string(i), i);
    }
    
    for (int i = 0; i < 200; ++i) {
        int* val = map.get("item_" + std::to_string(i));
        ASSERT_NE(val, nullptr);
        EXPECT_EQ(*val, i);
    }
}
