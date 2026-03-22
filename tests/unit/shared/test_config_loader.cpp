#include "ConfigLoader.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

class ConfigLoaderRouterTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char *existing_home = std::getenv("HOME");
    if (existing_home) {
      original_home_ = existing_home;
    }
    auto unique = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
    temp_home_ = std::filesystem::temp_directory_path() /
                 ("firmius_config_router_test_" + unique);
    std::filesystem::create_directories(temp_home_);
    setenv("HOME", temp_home_.c_str(), 1);
  }

  void TearDown() override {
    std::filesystem::remove_all(temp_home_);
    if (original_home_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", original_home_.c_str(), 1);
    }
  }

  std::filesystem::path temp_home_;
  std::string original_home_;
};

TEST_F(ConfigLoaderRouterTest, SavesAndLoadsRouterCategoriesAndPurposeRoutes) {
  auto &loader = firmius::shared::ConfigLoader::instance();

  firmius::shared::UserConfig cfg;
  cfg.defaultProviderId = "openai";
  cfg.defaultModelId = "gpt-5";
  cfg.defaultModelVariant = "thinking";
  cfg.modelRouterCategories["code"] = {"openai", "gpt-5-codex", "thinking"};
  cfg.modelRouterCategories["research"] = {"openrouter", "qwen-omni", ""};
  cfg.purposeRoutes["executor"] = "code";
  cfg.defaultRouteCategory = "research";
  cfg.enableSubagentRouteFallback = true;
  cfg.subagentRouteFallbackOrder = {"research", "code"};

  loader.updateConfig(cfg);
  loader.save();

  loader.updateConfig(firmius::shared::UserConfig{});
  loader.load();
  const auto loaded = loader.getConfig();

  ASSERT_EQ(loaded.modelRouterCategories.size(), 2u);
  ASSERT_EQ(loaded.purposeRoutes.size(), 1u);
  EXPECT_EQ(loaded.modelRouterCategories.at("code").providerId, "openai");
  EXPECT_EQ(loaded.modelRouterCategories.at("code").modelId, "gpt-5-codex");
  EXPECT_EQ(loaded.modelRouterCategories.at("code").variantName, "thinking");
  EXPECT_EQ(loaded.purposeRoutes.at("executor"), "code");
  EXPECT_EQ(loaded.defaultRouteCategory, "research");
  EXPECT_TRUE(loaded.enableSubagentRouteFallback);
  ASSERT_EQ(loaded.subagentRouteFallbackOrder.size(), 2u);
  EXPECT_EQ(loaded.subagentRouteFallbackOrder[0], "research");
  EXPECT_EQ(loaded.subagentRouteFallbackOrder[1], "code");
}

TEST_F(ConfigLoaderRouterTest, MissingRouterFieldsRemainBackwardCompatible) {
  auto &loader = firmius::shared::ConfigLoader::instance();
  const auto config_path = loader.getConfigPath();
  std::filesystem::create_directories(
      std::filesystem::path(config_path).parent_path());

  std::ofstream out(config_path);
  out << R"({
  "defaultProviderId": "openai",
  "defaultModelId": "gpt-5",
  "defaultModelVariant": "thinking",
  "defaultLeadPersona": "lead"
})";
  out.close();

  loader.updateConfig(firmius::shared::UserConfig{});
  loader.load();
  const auto loaded = loader.getConfig();

  EXPECT_TRUE(loaded.modelRouterCategories.empty());
  EXPECT_TRUE(loaded.purposeRoutes.empty());
  EXPECT_TRUE(loaded.defaultRouteCategory.empty());
  EXPECT_TRUE(loaded.enableSubagentRouteFallback);
  EXPECT_TRUE(loaded.subagentRouteFallbackOrder.empty());
  EXPECT_EQ(loaded.defaultProviderId, "openai");
}

} // namespace
