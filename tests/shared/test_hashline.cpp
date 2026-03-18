#include "utils/Hashline.hpp"

#include <gtest/gtest.h>

using namespace firmius::shared::utils;

TEST(Hashline, FormatsAndParsesAnchor) {
  const std::string anchor = Hashline::formatAnchor(12, "let value = x + 2;");
  auto parsed = Hashline::parseAnchor(anchor);

  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->lineNumber, 12);
  EXPECT_EQ(parsed->hash, Hashline::computeHash("let value = x + 2;"));
}

TEST(Hashline, EnhancesReadOutputWithMatchingAnchors) {
  const std::string content = "alpha\nbeta\n";
  const std::string enhanced = HashlineReadEnhancer::enhance(content);

  EXPECT_NE(enhanced.find(Hashline::formatLine(1, "alpha")), std::string::npos);
  EXPECT_NE(enhanced.find(Hashline::formatLine(2, "beta")), std::string::npos);
}
