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

TEST(Hashline, ParsesReadFormattedAnchorPrefix) {
  const std::string line = Hashline::formatLine(7, "use crate::compiler::module::ModuleResolver;");
  auto parsed = Hashline::parseAnchor(line);

  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->lineNumber, 7);
  EXPECT_EQ(parsed->hash, Hashline::computeHash("use crate::compiler::module::ModuleResolver;"));
}

TEST(Hashline, ParsesAnchorWithTrailingPipeOnly) {
  const std::string anchor = Hashline::formatAnchor(15, "beta") + "|";
  auto parsed = Hashline::parseAnchor(anchor);

  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->lineNumber, 15);
  EXPECT_EQ(parsed->hash, Hashline::computeHash("beta"));
}

TEST(Hashline, EnhancesReadOutputWithMatchingAnchors) {
  const std::string content = "alpha\nbeta\n";
  const std::string enhanced = HashlineReadEnhancer::enhance(content);

  EXPECT_NE(enhanced.find(Hashline::formatLine(1, "alpha")), std::string::npos);
  EXPECT_NE(enhanced.find(Hashline::formatLine(2, "beta")), std::string::npos);
}

TEST(HashlineTrimmer, TrimsValidPrefixes) {
  EXPECT_EQ(HashlineTrimmer::trimLine("15#1234|use crate::..."), "use crate::...");
  EXPECT_EQ(HashlineTrimmer::trimLine("622#3f69|};"), "};");
  EXPECT_EQ(HashlineTrimmer::trimLine("637#3db3|48a8|}"), "}");
  EXPECT_EQ(HashlineTrimmer::trimLine("1#aaaa|"), "");
}

TEST(HashlineTrimmer, IgnoresInvalidPrefixes) {
  EXPECT_EQ(HashlineTrimmer::trimLine("just some text"), "just some text");
  EXPECT_EQ(HashlineTrimmer::trimLine("15#use crate::..."), "15#use crate::...");
  EXPECT_EQ(HashlineTrimmer::trimLine("no hash|here"), "no hash|here");
  EXPECT_EQ(HashlineTrimmer::trimLine("15#12G4|not hex hash"), "15#12G4|not hex hash");
  EXPECT_EQ(HashlineTrimmer::trimLine("1234|just pipe"), "1234|just pipe");
  EXPECT_EQ(HashlineTrimmer::trimLine("aaaa|hex pipe"), "aaaa|hex pipe");
}

TEST(HashlineTrimmer, SanitizeContent) {
  HashlineTrimmer::SanitationResult res;
  char p1[] = "15#1234";
  char p2[] = "637#3db3";
  char p3[] = "48a8";
  std::string content = std::string(p1) + "|+ added line\n" + std::string(p2) + "|" + std::string(p3) + "|- removed line\nnormal line";
  EXPECT_EQ(HashlineTrimmer::sanitizeContent(content, &res), "added line\nremoved line\nnormal line");
  EXPECT_EQ(res.hashlinePrefixesStripped, 2);
  EXPECT_EQ(res.malformedHashFragmentsStripped, 1);
  EXPECT_EQ(res.diffMarkersStripped, 2);
}

TEST(HashlineTrimmer, SanitizeContentRecursive) {
  char p1[] = "1#1234";
  char p2[] = "5#abcd";
  char p3[] = "6#ef01";
  std::string input1 = std::string(p1) + "|" + std::string(p2) + "|" + std::string(p3) + "|foo";
  EXPECT_EQ(HashlineTrimmer::trimLine(input1), "foo");
}

TEST(HashlineTrimmer, DetectsSuspiciousMetadataPrefixes) {
  EXPECT_TRUE(HashlineTrimmer::startsWithSuspiciousMetadata("12#abcd|foo"));
  EXPECT_TRUE(HashlineTrimmer::startsWithSuspiciousMetadata("48a8|}"));
  EXPECT_FALSE(HashlineTrimmer::startsWithSuspiciousMetadata("let value = 48a8 | mask;"));
}

TEST(HashlineTrimmer, DetectsSuspiciousDiffJunk) {
  EXPECT_TRUE(HashlineTrimmer::startsWithSuspiciousDiffJunk("+++ b/file.cpp"));
  EXPECT_TRUE(HashlineTrimmer::startsWithSuspiciousDiffJunk("@@ -2,4 +2,5 @@"));
  EXPECT_TRUE(HashlineTrimmer::startsWithSuspiciousDiffJunk("+added_line"));
  EXPECT_TRUE(HashlineTrimmer::startsWithSuspiciousDiffJunk("-removed_line"));
  EXPECT_FALSE(HashlineTrimmer::startsWithSuspiciousDiffJunk("++counter;"));
  EXPECT_FALSE(HashlineTrimmer::startsWithSuspiciousDiffJunk("--counter;"));
}
