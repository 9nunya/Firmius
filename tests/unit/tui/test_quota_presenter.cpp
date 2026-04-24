#include "tui/QuotaPresenter.hpp"

#include <gtest/gtest.h>

#include "Enums.hpp"
#include "IProvider.hpp"

namespace firmius::tui::quota {
namespace {

class MockProvider final : public firmius::provider::IProvider {
public:
  explicit MockProvider(std::string id) : id_(std::move(id)) {}

  std::string getId() const override { return id_; }
  void stream(const firmius::shared::AgentHistory &,
              const firmius::provider::ProviderOptions &,
              std::function<void(const firmius::shared::StreamEvent &)>) override {}
  std::vector<firmius::shared::ModelInfo> listModels() override { return {}; }
  firmius::shared::ModelInfo getModelInfo(const std::string &) override {
    return {};
  }
  void generateSummary(
      const std::string &, const firmius::shared::AgentHistory &,
      const std::string &,
      std::function<void(const firmius::shared::StreamEvent &)>,
      std::atomic<bool> * = nullptr) override {}
  firmius::provider::ProviderType getProviderType() const override {
    return firmius::provider::ProviderType::APIKey;
  }
  bool isConfigured() const override { return true; }

private:
  std::string id_;
};

using firmius::shared::QuotaBucket;

TEST(QuotaPresenterTest, ReturnsEmptyForNullProvider) {
  std::vector<QuotaBucket> buckets;
  EXPECT_EQ(format(std::shared_ptr<firmius::provider::IProvider>{}, "some-model",
                   buckets),
            "");
}

TEST(QuotaPresenterTest, ReturnsEmptyForEmptyBuckets) {
  auto provider = std::make_shared<MockProvider>("test");
  std::vector<QuotaBucket> buckets;
  EXPECT_EQ(format(provider, "some-model", buckets), "");
}

TEST(QuotaPresenterTest, DefaultPresenterSelectsExactMatch) {
  auto provider = std::make_shared<MockProvider>("antigravity");
  std::vector<QuotaBucket> buckets = {
      {"gpt-4", 0.75f, "", ""},
      {"claude", 0.50f, "", ""},
  };
  EXPECT_EQ(format(provider, "gpt-4", buckets), "󰆧 75%");
}

TEST(QuotaPresenterTest, DefaultPresenterSelectsPartialMatch) {
  auto provider = std::make_shared<MockProvider>("openrouter");
  std::vector<QuotaBucket> buckets = {
      {"gemini-pro 5h", 0.85f, "", ""},
      {"gemini-pro weekly", 0.92f, "", ""},
  };
  const std::string result = format(provider, "gemini-pro", buckets);
  EXPECT_NE(result.find("85%"), std::string::npos);
}

TEST(QuotaPresenterTest, DefaultPresenterFallsBackToFirstBucket) {
  auto provider = std::make_shared<MockProvider>("some-provider");
  std::vector<QuotaBucket> buckets = {
      {"bucket1", 0.33f, "", ""},
      {"bucket2", 0.66f, "", ""},
  };
  EXPECT_EQ(format(provider, "unknown-model", buckets), "󰆧 33%");
}

TEST(QuotaPresenterTest, CodexPresenterShowsFiveHourAndWeeklyBuckets) {
  auto provider = std::make_shared<MockProvider>("codex");
  std::vector<QuotaBucket> buckets = {
      {"codex 5h limit", 0.85f, "", ""},
      {"codex weekly limit", 0.92f, "", ""},
  };
  const std::string result = format(provider, "codex", buckets);
  EXPECT_NE(result.find("85%"), std::string::npos);
  EXPECT_NE(result.find("92%"), std::string::npos);
  EXPECT_NE(result.find("󱑂"), std::string::npos);
  EXPECT_NE(result.find("󰃭"), std::string::npos);
}

TEST(QuotaPresenterTest, CodexPresenterFallsBackWhenSpecialBucketsMissing) {
  auto provider = std::make_shared<MockProvider>("codex");
  std::vector<QuotaBucket> buckets = {{"some-other-bucket", 0.45f, "", ""}};
  const std::string result = format(provider, "codex", buckets);
  EXPECT_NE(result.find("45%"), std::string::npos);
}

TEST(QuotaPresenterTest, HandlesNormalizedNames) {
  auto provider = std::make_shared<MockProvider>("test");
  std::vector<QuotaBucket> buckets = {{"GPT-4", 0.55f, "", ""}};
  const std::string result = format(provider, "gpt_4", buckets);
  EXPECT_NE(result.find("55%"), std::string::npos);
}

} // namespace
} // namespace firmius::tui::quota
