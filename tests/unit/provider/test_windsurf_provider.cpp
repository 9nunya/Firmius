#include "providers/BaseOAuthProvider.hpp"
#include "providers/WindsurfModels.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#define private public
#include "providers/WindsurfProvider.hpp"
#undef private

using firmius::provider::WindsurfProvider;

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

class WindsurfProviderTest : public ::testing::Test {
protected:
  void SetUp() override {
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_windsurf_provider_" +
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

  std::filesystem::path testHome_;
  std::unique_ptr<ScopedHomeOverride> homeOverride_;
};

TEST_F(WindsurfProviderTest, PersistsCascadeStepOffsetAcrossProviderInstances) {
  {
    WindsurfProvider provider;
    provider.setCascadeForThread("thread-a", "cascade-a", "csrf-a", 7);
    provider.setCascadeStepOffsetForThread("thread-a", 11);

    auto entry = provider.getCascadeForThread("thread-a", "csrf-a");
    EXPECT_EQ(entry.cascadeId, "cascade-a");
    EXPECT_EQ(entry.csrf, "csrf-a");
    EXPECT_EQ(entry.stepOffset, 11u);
  }

  WindsurfProvider reloaded;
  auto entry = reloaded.getCascadeForThread("thread-a", "csrf-a");
  EXPECT_EQ(entry.cascadeId, "cascade-a");
  EXPECT_EQ(entry.csrf, "csrf-a");
  EXPECT_EQ(entry.stepOffset, 11u);
}

TEST_F(WindsurfProviderTest, DropsCascadeWhenCsrfBelongsToOldLspBoot) {
  WindsurfProvider provider;
  provider.setCascadeForThread("thread-a", "cascade-a", "csrf-old", 5);

  auto stale = provider.getCascadeForThread("thread-a", "csrf-new");
  EXPECT_TRUE(stale.cascadeId.empty());
  EXPECT_EQ(stale.stepOffset, 0u);

  auto missing = provider.getCascadeForThread("thread-a", "csrf-old");
  EXPECT_TRUE(missing.cascadeId.empty());
}

TEST_F(WindsurfProviderTest, LoadsLegacyCascadeEntriesWithZeroStepOffset) {
  const auto path = testHome_ / ".firmius" / "windsurf_cascades.json";
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << R"({"thread-a":{"cascade":"cascade-a","csrf":"csrf-a"}})";
  f.close();

  WindsurfProvider provider;
  auto entry = provider.getCascadeForThread("thread-a", "csrf-a");
  EXPECT_EQ(entry.cascadeId, "cascade-a");
  EXPECT_EQ(entry.csrf, "csrf-a");
  EXPECT_EQ(entry.stepOffset, 0u);
}

} // namespace
