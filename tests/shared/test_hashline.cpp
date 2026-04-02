#include "utils/Hashline.hpp"

#include <filesystem>
#include <fstream>
#include <set>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace firmius::shared::utils;

namespace {

std::filesystem::path findRepoRoot() {
  auto current = std::filesystem::current_path();
  while (!current.empty()) {
    if (std::filesystem::exists(current / "packages" / "shared" / "include" /
                                "utils" / "Hashline.hpp")) {
      return current;
    }
    if (current == current.root_path()) {
      break;
    }
    current = current.parent_path();
  }
  throw std::runtime_error("Unable to locate repository root");
}

std::string readTextFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Unable to open " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::vector<std::string> splitLines(const std::string &content) {
  std::vector<std::string> lines;
  size_t start = 0;
  size_t end = content.find('\n');
  while (end != std::string::npos) {
    lines.emplace_back(content.substr(start, end - start));
    start = end + 1;
    end = content.find('\n', start);
  }
  if (start < content.size()) {
    lines.emplace_back(content.substr(start));
  }
  return lines;
}

void expectUniqueHashesForFile(const std::filesystem::path &root,
                               const std::filesystem::path &relativePath) {
  const auto fullPath = root / relativePath;
  const auto content = readTextFile(fullPath);
  const auto lines = splitLines(content);
  ASSERT_FALSE(lines.empty()) << fullPath.string();

  const auto hashes = Hashline::computeLineHashes(lines);
  ASSERT_EQ(hashes.size(), lines.size()) << fullPath.string();

  std::set<std::string> uniqueHashes(hashes.begin(), hashes.end());
  EXPECT_EQ(uniqueHashes.size(), hashes.size()) << fullPath.string();
}

} // namespace

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
  const std::vector<std::string> lines = {"alpha", "beta"};

  EXPECT_NE(enhanced.find(Hashline::formatLine(lines, 1)), std::string::npos);
  EXPECT_NE(enhanced.find(Hashline::formatLine(lines, 2)), std::string::npos);
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
  EXPECT_FALSE(HashlineTrimmer::startsWithSuspiciousDiffJunk("--counter;"));
}

TEST(Hashline, DistinctHashesForWhitespace) {
    const std::string h_empty = Hashline::computeHash("");
    const std::string h_space = Hashline::computeHash(" ");
    const std::string h_tab = Hashline::computeHash("\t");
    const std::string h_spaces = Hashline::computeHash("  ");

    EXPECT_NE(h_empty, h_space);
    EXPECT_NE(h_empty, h_tab);
    EXPECT_NE(h_empty, h_spaces);
    EXPECT_NE(h_space, h_tab);
    EXPECT_NE(h_space, h_spaces);
    EXPECT_NE(h_tab, h_spaces);
}

TEST(Hashline, ResolveAnchorCollisions) {
    std::vector<std::string> lines = {
        "aaaa",
        "bbbb",
        "cccc",
        "bbbb",
        "dddd"
    };

    const std::string anchor = Hashline::formatAnchor(lines, 2);
    const std::string duplicateAnchor = Hashline::formatAnchor(lines, 4);

    EXPECT_NE(anchor, duplicateAnchor);

    AnchorResult res = Hashline::resolveAnchor(lines, "2", 15);
    EXPECT_EQ(res.status, AnchorResult::Status::SUCCESS);
    EXPECT_EQ(res.lineIndex, 1);
    EXPECT_FALSE(res.relocated);
}

TEST(Hashline, ResolveAnchorRelocatesAcrossNearbyInsertions) {
    std::vector<std::string> lines = {
        "header",
        "}",
        "}",
        "footer"
    };
    const std::string anchor = Hashline::formatAnchor(lines, 3);

    std::vector<std::string> shifted = {
        "intro",
        "header",
        "}",
        "}",
        "footer"
    };

    AnchorResult res = Hashline::resolveAnchor(shifted, "4", 15);
    EXPECT_EQ(res.status, AnchorResult::Status::SUCCESS);
    EXPECT_EQ(res.lineIndex, 3);
    EXPECT_FALSE(res.relocated);
}

TEST(Hashline, ComputesUniqueHashesForRepeatedEmptyAndBraceLines) {
  const std::vector<std::string> lines = {
      "",
      "",
      "}",
      "}",
      "  ",
      "  ",
      "tail",
  };

  const auto hashes = Hashline::computeLineHashes(lines);
  ASSERT_EQ(hashes.size(), lines.size());

  std::set<std::string> uniqueHashes(hashes.begin(), hashes.end());
  EXPECT_EQ(uniqueHashes.size(), hashes.size());
}

TEST(Hashline, RealProjectFilesProduceUniqueLineHashes) {
  const auto root = findRepoRoot();

  expectUniqueHashesForFile(root, "packages/core/src/Engine.cpp");
  expectUniqueHashesForFile(root, "packages/core/src/tools/FileEditTool.cpp");
  expectUniqueHashesForFile(root, "packages/shared/src/utils/Hashline.cpp");
  expectUniqueHashesForFile(root, "packages/provider/CMakeLists.txt");
}

TEST(Hashline, ExtractsLeadingLineNumberFromAnchors) {
    std::vector<std::string> lines = {"alpha", "beta", "gamma"};
    
    // Old style anchor with hash
    AnchorResult res1 = Hashline::resolveAnchor(lines, "2#abcd", 15);
    EXPECT_EQ(res1.status, AnchorResult::Status::SUCCESS);
    EXPECT_EQ(res1.lineIndex, 1);

    // Anchor with trailing content prefix
    AnchorResult res2 = Hashline::resolveAnchor(lines, "3|gamma", 15);
    EXPECT_EQ(res2.status, AnchorResult::Status::SUCCESS);
    EXPECT_EQ(res2.lineIndex, 2);

    // Malformed anchor starting with non-digits should still fail
    AnchorResult res3 = Hashline::resolveAnchor(lines, "abc2", 15);
    EXPECT_EQ(res3.status, AnchorResult::Status::NOT_NUMERIC);
}
