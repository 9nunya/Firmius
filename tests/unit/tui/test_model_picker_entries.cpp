#include "utils/ModelPickerEntries.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::shared::ModelInfo;
using firmius::shared::ModelVariant;

TEST(ModelPickerEntriesTest, BuildsDefaultAndVariantRows) {
  ModelInfo model;
  model.provider = "openai";
  model.id = "gpt-5";
  model.variants = {ModelVariant{"low", "{}"}, ModelVariant{"high", "{}"}};
  model.contextWindow = 200000;
  model.modalities = {"text", "image"};

  const auto entries =
      firmius::tui::BuildModelPickerEntries({model}, true);

  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].provider_id, "openai");
  EXPECT_EQ(entries[0].model_id, "gpt-5");
  EXPECT_EQ(entries[0].variant_name, "");
  EXPECT_EQ(entries[0].title, "GPT 5");
  EXPECT_EQ(entries[0].provider_label, "openai");
  EXPECT_NE(entries[0].meta_label.find("200K ctx"), std::string::npos);
  EXPECT_NE(entries[0].meta_label.find("Default variant"), std::string::npos);
  EXPECT_NE(entries[0].meta_label.find("Vision"), std::string::npos);
}

TEST(ModelPickerEntriesTest, FiltersAcrossModelAndVariantTokens) {
  ModelInfo model;
  model.provider = "openai";
  model.id = "gpt-5";
  model.variants = {ModelVariant{"low", "{}"}, ModelVariant{"high", "{}"}};
  const auto entries = firmius::tui::BuildModelPickerEntries({model}, true);

  const auto filtered =
      firmius::tui::FilterModelPickerEntries(entries, "gpt-5 high");
  ASSERT_EQ(filtered.size(), 1u);
  EXPECT_EQ(entries[filtered.front()].variant_name, "high");
}

TEST(ModelPickerEntriesTest, FiltersUsingPrettifiedAndProviderTokens) {
  ModelInfo model;
  model.provider = "antigravity";
  model.id = "claude-opus-4-6-thinking";
  model.contextWindow = 200000;
  model.supportsReasoning = true;
  model.variants = {ModelVariant{"max", "{}"}};

  const auto entries = firmius::tui::BuildModelPickerEntries({model}, true);

  const auto filtered = firmius::tui::FilterModelPickerEntries(
      entries, "claude opus 4.6 ant max");
  ASSERT_EQ(filtered.size(), 1u);
  EXPECT_EQ(entries[filtered.front()].provider_id, "antigravity");
}

} // namespace
