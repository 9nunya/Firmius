#include "providers/CodexProvider.hpp"

#include <gtest/gtest.h>
#include <vector>

using firmius::provider::CodexProvider;

namespace {

bool containsModel(const std::vector<firmius::shared::ModelInfo> &models,
                   const std::string &id) {
  for (const auto &model : models) {
    if (model.id == id) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST(CodexProvider, StaticModelCatalogIncludesNewCodexUiModels) {
  CodexProvider provider;
  const auto models = provider.listModels();

  EXPECT_EQ(models.size(), 9u);
  EXPECT_TRUE(containsModel(models, "gpt-5.4"));
  EXPECT_TRUE(containsModel(models, "gpt-5.4-mini"));
  EXPECT_TRUE(containsModel(models, "gpt-5.3-codex"));
  EXPECT_TRUE(containsModel(models, "gpt-5.2-codex"));
  EXPECT_TRUE(containsModel(models, "gpt-5.2"));
  EXPECT_TRUE(containsModel(models, "gpt-5.1-codex-max"));
  EXPECT_TRUE(containsModel(models, "gpt-5.1-codex-mini"));
}

TEST(CodexProvider, ModelInfoNormalizesAndPreservesVariantMetadata) {
  CodexProvider provider;

  const auto gpt54 = provider.getModelInfo("openai/gpt-5.4");
  EXPECT_EQ(gpt54.id, "gpt-5.4");
  ASSERT_EQ(gpt54.variants.size(), 5u);
  EXPECT_EQ(gpt54.variants.front().variantName, "none");
  EXPECT_EQ(gpt54.variants.back().variantName, "xhigh");

  const auto gpt54Mini = provider.getModelInfo("codex/gpt-5.4-mini");
  EXPECT_EQ(gpt54Mini.id, "gpt-5.4-mini");
  ASSERT_EQ(gpt54Mini.variants.size(), 2u);
  EXPECT_EQ(gpt54Mini.variants.front().variantName, "medium");
  EXPECT_EQ(gpt54Mini.variants.back().variantName, "high");

  const auto gpt53Codex = provider.getModelInfo("chatgpt/gpt-5.3-codex");
  EXPECT_EQ(gpt53Codex.id, "gpt-5.3-codex");
  ASSERT_EQ(gpt53Codex.variants.size(), 3u);
  EXPECT_EQ(gpt53Codex.variants.front().variantName, "low");
  EXPECT_EQ(gpt53Codex.variants.back().variantName, "high");
}
