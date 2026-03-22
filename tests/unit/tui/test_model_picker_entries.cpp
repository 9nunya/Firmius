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

  const auto entries =
      firmius::tui::BuildModelPickerEntries({model}, true);

  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].provider_id, "openai");
  EXPECT_EQ(entries[0].model_id, "gpt-5");
  EXPECT_EQ(entries[0].variant_name, "");
  EXPECT_NE(entries[0].label.find("default variant"), std::string::npos);
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

} // namespace
