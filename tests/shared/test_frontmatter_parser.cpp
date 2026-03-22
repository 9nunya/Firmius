#include "utils/FrontmatterParser.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::shared::FrontmatterParser;
using firmius::shared::FrontmatterDocument;

TEST(FrontmatterParserTest, ParsesQuotedAndUnquotedScalars) {
  const auto doc = FrontmatterParser::parseMarkdown(
      R"md(---
name: lead
title: "Lead Persona"
description: 'Primary worker'
work_role: "executor"
---
Body text.
)md",
      "quoted-scalars.md");

  EXPECT_EQ(FrontmatterParser::getString(doc, "name").value(), "lead");
  EXPECT_EQ(FrontmatterParser::getString(doc, "title").value(), "Lead Persona");
  EXPECT_EQ(FrontmatterParser::getString(doc, "description").value(),
            "Primary worker");
  EXPECT_EQ(FrontmatterParser::getString(doc, "work_role").value(),
            "executor");
  EXPECT_EQ(doc.body, "Body text.\n");
}

TEST(FrontmatterParserTest, ParsesBooleansAndArrayValues) {
  const auto values = FrontmatterParser::parse(
      R"fm(
# comment
switchable: yes
canSpawn: 0
scopes: ["FilesystemRead", "Semantic"]
)fm",
      "booleans-arrays.md");

  FrontmatterDocument doc;
  doc.values = values;

  EXPECT_EQ(FrontmatterParser::getBool(doc, "switchable").value(), true);
  EXPECT_EQ(FrontmatterParser::getBool(doc, "canSpawn").value(), false);
  const auto scopes = FrontmatterParser::getStringArray(doc, "scopes");
  ASSERT_EQ(scopes.size(), 2u);
  EXPECT_EQ(scopes[0], "FilesystemRead");
  EXPECT_EQ(scopes[1], "Semantic");
}

TEST(FrontmatterParserTest, SupportsSingleQuotedArrayElements) {
  const auto doc = FrontmatterParser::parseMarkdown(
      R"md(---
scopes: ['lead', 'executor']
---
Body text.
)md",
      "single-quoted-array.md");

  const auto scopes = FrontmatterParser::getStringArray(doc, "scopes");
  ASSERT_EQ(scopes.size(), 2u);
  EXPECT_EQ(scopes[0], "lead");
  EXPECT_EQ(scopes[1], "executor");
}

TEST(FrontmatterParserTest, RejectsMalformedArrays) {
  EXPECT_THROW(
      FrontmatterParser::parse(R"fm(scopes: ["FilesystemRead",)fm",
                               "malformed-array.md"),
      std::runtime_error);
}

TEST(FrontmatterParserTest, SkipsCommentsAndBlankLines) {
  const auto doc = FrontmatterParser::parseMarkdown(
      R"md(---

# purpose metadata
title: Commented

switchable: false
---
Body text.
)md",
      "comments-and-blanks.md");

  EXPECT_EQ(FrontmatterParser::getString(doc, "title").value(), "Commented");
  EXPECT_EQ(FrontmatterParser::getBool(doc, "switchable").value(), false);
  EXPECT_EQ(doc.body, "Body text.\n");
}

} // namespace
