#include "providers/OpenRouterProvider.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>

#include <gtest/gtest.h>

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

class TestOpenRouterProvider : public firmius::provider::OpenRouterProvider {
public:
  TestOpenRouterProvider() : OpenRouterProvider("") {}

  using firmius::provider::OpenRouterProvider::getAllQuotas;
  using firmius::provider::OpenRouterProvider::refreshQuotas;

  void setQuotaInfo(const std::string &identifier,
                    const firmius::provider::OpenRouterProvider::KeyQuotaInfo &info) {
    quotaInfo_[identifier] = info;
  }

protected:
  std::optional<firmius::provider::OpenRouterProvider::KeyQuotaInfo>
  fetchKeyQuotaInfo(const firmius::shared::APIKeyAccount &acc) const override {
    auto it = quotaInfo_.find(acc.identifier);
    if (it == quotaInfo_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

private:
  std::map<std::string, firmius::provider::OpenRouterProvider::KeyQuotaInfo>
      quotaInfo_;
};

} // namespace

TEST(OpenRouterProvider, ParsesLiveKeyEndpointShape) {
  const std::string body = R"({
    "data": {
      "byok_usage": 0,
      "byok_usage_daily": 0,
      "byok_usage_monthly": 0,
      "byok_usage_weekly": 0,
      "creator_user_id": "user_3BjeooIgeyQhfIJSz1gbHMVYWoz",
      "expires_at": null,
      "include_byok_in_limit": false,
      "is_free_tier": true,
      "is_management_key": false,
      "is_provisioning_key": false,
      "label": "sk-or-v1-931...a08",
      "limit": null,
      "limit_remaining": null,
      "limit_reset": null,
      "rate_limit": {
        "interval": "10s",
        "note": "This field is deprecated and safe to ignore.",
        "requests": -1
      },
      "usage": 0,
      "usage_daily": 0,
      "usage_monthly": 0,
      "usage_weekly": 0
    }
  })";

  auto parsed =
      firmius::provider::OpenRouterProvider::parseKeyQuotaInfoResponse(body);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->limit.has_value());
  EXPECT_FALSE(parsed->limitRemaining.has_value());
  EXPECT_TRUE(parsed->limitReset.empty());
  EXPECT_DOUBLE_EQ(parsed->usage, 0.0);
  EXPECT_EQ(parsed->label, "sk-or-v1-931...a08");
  EXPECT_TRUE(parsed->isFreeTier);
}

TEST(OpenRouterProvider, RefreshQuotasPersistsQuotaBucketsForApiKeys) {
  const auto tempHome = std::filesystem::temp_directory_path() /
                        "firmius_openrouter_quota_test_home";
  std::filesystem::remove_all(tempHome);
  std::filesystem::create_directories(tempHome);
  ScopedHomeOverride scopedHome(tempHome);

  TestOpenRouterProvider provider;
  provider.addApiKey("sk-or-v1-first");
  provider.addApiKey("sk-or-v1-second");

  provider.setQuotaInfo("Key #1",
                        {.limit = 10.0,
                         .limitRemaining = 2.5,
                         .limitReset = "2026-04-01T12:00:00Z",
                         .usage = 7.5,
                         .label = "first",
                         .isFreeTier = false});
  provider.setQuotaInfo("Key #2",
                        {.limit = std::nullopt,
                         .limitRemaining = std::nullopt,
                         .limitReset = "",
                         .usage = 0.0,
                         .label = "second",
                         .isFreeTier = true});

  provider.refreshQuotas();

  const auto quotas = provider.getAllQuotas();
  ASSERT_EQ(quotas.size(), 2u);

  auto first = quotas.find("Key #1");
  ASSERT_NE(first, quotas.end());
  ASSERT_EQ(first->second.size(), 1u);
  EXPECT_EQ(first->second.front().name, "openrouter");
  EXPECT_NEAR(first->second.front().remainingFraction, 0.25f, 0.0001f);
  EXPECT_EQ(first->second.front().resetTime, "2026-04-01T12:00:00Z");

  auto second = quotas.find("Key #2");
  ASSERT_NE(second, quotas.end());
  ASSERT_EQ(second->second.size(), 1u);
  EXPECT_EQ(second->second.front().name, "openrouter");
  EXPECT_FLOAT_EQ(second->second.front().remainingFraction, 0.0f);
  EXPECT_EQ(second->second.front().resetTime,
            "Free tier key (remaining daily quota unavailable)");

  auto accounts = provider.getAccounts();
  ASSERT_EQ(accounts.size(), 2u);
  ASSERT_TRUE(accounts[0].metadata.count("quota:openrouter"));
  EXPECT_NEAR(std::stof(accounts[0].metadata.at("quota:openrouter")), 0.25f,
              0.0001f);
  EXPECT_EQ(accounts[0].metadata.at("quota_limit:openrouter"), "10.000000");
  EXPECT_EQ(accounts[0].metadata.at("quota_remaining:openrouter"), "2.500000");
  EXPECT_EQ(accounts[1].metadata.at("quota:openrouter"), "0");
  EXPECT_EQ(accounts[1].metadata.at("quota_note:openrouter"),
            "Free tier key (remaining daily quota unavailable)");
}
