#include "providers/BaseOAuthProvider.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using firmius::provider::BaseOAuthProvider;
using firmius::provider::ProviderOptions;
using firmius::shared::AgentHistory;
using firmius::shared::ModelInfo;
using firmius::shared::OAuthAccount;
using firmius::shared::StreamEvent;

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

class TestBaseOAuthProvider : public BaseOAuthProvider {
public:
  TestBaseOAuthProvider() : BaseOAuthProvider("test-oauth") {}

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

  void stream(const AgentHistory &, const ProviderOptions &,
              std::function<void(const StreamEvent &)>) override {}

  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)>,
                       std::atomic<bool> *) override {}

  std::unique_ptr<firmius::OAuthWizard> beginConnectionWizard() override {
    return nullptr;
  }

  bool refreshAccessToken(OAuthAccount &) override { return true; }

  void refreshQuotas() override {}
};

class BaseOAuthProviderTest : public ::testing::Test {
protected:
  void SetUp() override {
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_base_oauth_provider_" +
                 std::to_string(
                     ::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(testHome_);
    std::filesystem::create_directories(testHome_ / ".firmius");
    homeOverride_ = std::make_unique<ScopedHomeOverride>(testHome_);
  }

  void TearDown() override {
    homeOverride_.reset();
    std::filesystem::remove_all(testHome_);
  }

  OAuthAccount makeAccount(const std::string &identifier) {
    OAuthAccount account;
    account.identifier = identifier;
    account.refreshToken = "refresh-" + identifier;
    account.accessToken = "access-" + identifier;
    account.tokenExpiration = std::numeric_limits<int64_t>::max();
    return account;
  }

  std::filesystem::path testHome_;
  std::unique_ptr<ScopedHomeOverride> homeOverride_;
};

TEST_F(BaseOAuthProviderTest, GetAccountsReturnsStableSnapshotCopy) {
  TestBaseOAuthProvider provider;
  provider.addAccount(makeAccount("first@example.com"));

  const auto &snapshot = provider.getAccounts();
  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(snapshot.front().identifier, "first@example.com");

  provider.addAccount(makeAccount("second@example.com"));

  EXPECT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(snapshot.front().identifier, "first@example.com");

  const auto latest = provider.getAccounts();
  ASSERT_EQ(latest.size(), 2u);
  EXPECT_EQ(latest.back().identifier, "second@example.com");
}

TEST_F(BaseOAuthProviderTest, AddedAccountPersistsAcrossProviderRestart) {
  {
    TestBaseOAuthProvider provider;
    provider.addAccount(makeAccount("persisted@example.com"));

    const auto accounts = provider.getAccounts();
    ASSERT_EQ(accounts.size(), 1u);
    EXPECT_EQ(accounts.front().identifier, "persisted@example.com");
  }

  TestBaseOAuthProvider reloadedProvider;
  const auto reloadedAccounts = reloadedProvider.getAccounts();
  ASSERT_EQ(reloadedAccounts.size(), 1u);
  EXPECT_EQ(reloadedAccounts.front().identifier, "persisted@example.com");
  EXPECT_EQ(reloadedAccounts.front().refreshToken,
            "refresh-persisted@example.com");
  EXPECT_EQ(reloadedAccounts.front().accessToken,
            "access-persisted@example.com");
}

} // namespace
