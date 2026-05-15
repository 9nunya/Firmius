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
description: 'Primary coder'
work_role: "coder"
---
Body text.
)md",
      "quoted-scalars.md");

  EXPECT_EQ(FrontmatterParser::getString(doc, "name").value(), "lead");
  EXPECT_EQ(FrontmatterParser::getString(doc, "title").value(), "Lead Persona");
  EXPECT_EQ(FrontmatterParser::getString(doc, "description").value(),
            "Primary coder");
  EXPECT_EQ(FrontmatterParser::getString(doc, "work_role").value(),
            "coder");
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
scopes: ['lead', 'coder']
---
Body text.
)md",
      "single-quoted-array.md");

  const auto scopes = FrontmatterParser::getStringArray(doc, "scopes");
  ASSERT_EQ(scopes.size(), 2u);
  EXPECT_EQ(scopes[0], "lead");
  EXPECT_EQ(scopes[1], "coder");
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

TEST(FrontmatterParserTest, ParsesIntegers) {
  const auto doc = FrontmatterParser::parseMarkdown(
      R"md(---
priority: 42
negative: -10
---
Body text.
)md",
      "integers.md");

  EXPECT_EQ(FrontmatterParser::getInt(doc, "priority").value(), 42);
  EXPECT_EQ(FrontmatterParser::getInt(doc, "negative").value(), -10);
}

TEST(FrontmatterParserTest, ParsesListOfMaps) {
  const auto doc = FrontmatterParser::parseMarkdown(
      R"md(---
args:
  - name: arg1
    type: string
    optional: true
  - name: arg2
    type: number
    optional: false
---
Body text.
)md",
      "list-of-maps.md");

  auto args = FrontmatterParser::getArray(doc, "args");
  ASSERT_TRUE(args.has_value());
  ASSERT_EQ(args->size(), 2u);

  auto arg1 = std::get_if<firmius::shared::FrontmatterValue::Map>(&(*args)[0].value);
  ASSERT_NE(arg1, nullptr);
  EXPECT_EQ(std::get<std::string>((*arg1)["name"].value), "arg1");
  EXPECT_EQ(std::get<std::string>((*arg1)["type"].value), "string");
  EXPECT_EQ(std::get<bool>((*arg1)["optional"].value), true);

  auto arg2 = std::get_if<firmius::shared::FrontmatterValue::Map>(&(*args)[1].value);
  ASSERT_NE(arg2, nullptr);
  EXPECT_EQ(std::get<std::string>((*arg2)["name"].value), "arg2");
  EXPECT_EQ(std::get<std::string>((*arg2)["type"].value), "number");
  EXPECT_EQ(std::get<bool>((*arg2)["optional"].value), false);
}

} // namespace
