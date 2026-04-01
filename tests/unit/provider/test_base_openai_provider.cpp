#include "providers/BaseOpenAIProvider.hpp"

#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>

using firmius::provider::BaseOpenAIProvider;
using firmius::shared::ModelInfo;

namespace {

class ScopedHomeOverride {
public:
  explicit ScopedHomeOverride(const std::filesystem::path &home)
      : hadHome_(std::getenv("HOME") != nullptr),
        originalHome_(hadHome_ ? std::getenv("HOME") : "") {
    setenv("HOME", home.c_str(), 1);
  }

  ~ScopedHomeOverride() {
    if (hadHome_) {
      setenv("HOME", originalHome_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
  }

private:
  bool hadHome_ = false;
  std::string originalHome_;
};

class TestBaseOpenAIProvider : public BaseOpenAIProvider {
public:
  TestBaseOpenAIProvider()
      : BaseOpenAIProvider("test-openai", "https://example.invalid", "") {}

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = "test-model";
    model.provider = getId();
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }

  using BaseOpenAIProvider::handleRateLimitAndMaybeSwitch;
};

} // namespace

class BaseOpenAIProviderTest : public ::testing::Test {
protected:
  void SetUp() override {
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_base_openai_provider_" +
                 std::to_string(::testing::UnitTest::GetInstance()
                                    ->random_seed()));
    std::filesystem::remove_all(testHome_);
    std::filesystem::create_directories(testHome_);
    homeOverride_ = std::make_unique<ScopedHomeOverride>(testHome_);
  }

  void TearDown() override {
    homeOverride_.reset();
    std::filesystem::remove_all(testHome_);
  }

  std::filesystem::path testHome_;
  std::unique_ptr<ScopedHomeOverride> homeOverride_;
};

TEST_F(BaseOpenAIProviderTest, FormatErrorMessageIncludesContextAndRawBody) {
  const std::string body = R"({"error":"rate_limited"})";

  const std::string message = TestBaseOpenAIProvider::formatErrorMessage(
      "openrouter", "gpt-test", 429, body, "API error");

  EXPECT_NE(message.find("API error (HTTP 429)"), std::string::npos);
  EXPECT_NE(message.find("Provider: openrouter"), std::string::npos);
  EXPECT_NE(message.find("Model: gpt-test"), std::string::npos);
  EXPECT_NE(message.find("Raw provider body:\n" + body), std::string::npos);
}

TEST_F(BaseOpenAIProviderTest, RateLimitSwitchesToAnotherAvailableApiKey) {
  TestBaseOpenAIProvider provider;

  provider.addApiKey("key-one");
  provider.addApiKey("key-two");

  auto current = provider.getAvailableAccount();
  ASSERT_TRUE(current.has_value());
  ASSERT_NE(*current, nullptr);
  EXPECT_EQ((*current)->getIdentifier(), "Key #1");

  const auto result =
      provider.handleRateLimitAndMaybeSwitch(**current, std::nullopt, 0, 0);

  EXPECT_TRUE(result.switched);
  EXPECT_EQ(result.nextAccountIdentifier, "Key #2");

  auto accounts = provider.getAccounts();
  ASSERT_EQ(accounts.size(), 2u);
  EXPECT_TRUE(accounts[0].rateLimited);
  EXPECT_FALSE(accounts[1].rateLimited);
}

TEST_F(BaseOpenAIProviderTest,
       RateLimitDoesNotSwitchWhenAllApiKeysAreRateLimited) {
  TestBaseOpenAIProvider provider;

  provider.addApiKey("key-one");
  provider.addApiKey("key-two");

  auto first = provider.getAvailableAccount();
  ASSERT_TRUE(first.has_value());
  ASSERT_NE(*first, nullptr);

  const auto firstResult =
      provider.handleRateLimitAndMaybeSwitch(**first, std::nullopt, 0, 0);
  ASSERT_TRUE(firstResult.switched);
  EXPECT_EQ(firstResult.nextAccountIdentifier, "Key #2");

  auto second = provider.getAvailableAccount();
  ASSERT_TRUE(second.has_value());
  ASSERT_NE(*second, nullptr);
  EXPECT_EQ((*second)->getIdentifier(), "Key #2");

  const auto secondResult =
      provider.handleRateLimitAndMaybeSwitch(**second, std::nullopt, 0, 1);

  EXPECT_FALSE(secondResult.switched);
  EXPECT_TRUE(secondResult.nextAccountIdentifier.empty());

  auto accounts = provider.getAccounts();
  ASSERT_EQ(accounts.size(), 2u);
  EXPECT_TRUE(accounts[0].rateLimited);
  EXPECT_TRUE(accounts[1].rateLimited);
  EXPECT_FALSE(provider.getAvailableAccount().has_value());
}
