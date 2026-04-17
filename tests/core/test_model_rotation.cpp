#include "ConfigLoader.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "Panic.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;

namespace {

// ---------------------------------------------------------------------------
// Mock providers
// ---------------------------------------------------------------------------

/// Always emits a StreamError with configurable status code.
class FailingProvider : public IProvider {
public:
  FailingProvider(std::string id, int httpStatus = 401,
                  std::string errMsg = "auth error")
      : id_(std::move(id)), httpStatus_(httpStatus),
        errMsg_(std::move(errMsg)) {}

  std::string getId() const override { return id_; }

  void stream(const AgentHistory &, const ProviderOptions &,
              std::function<void(const StreamEvent &)> onEvent) override {
    callCount_.fetch_add(1);
    onEvent(StreamError{errMsg_, httpStatus_, ""});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo m;
    m.id = "fail-model";
    m.provider = id_;
    m.contextWindow = 4096;
    return {m};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }

  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> * = nullptr) override {
    onEvent(TextChunk{"summary"});
    onEvent(StreamDone{StopReason::Stop});
  }

  ProviderType getProviderType() const override {
    return ProviderType::APIKey;
  }

  int callCount() const { return callCount_.load(); }

private:
  std::string id_;
  int httpStatus_;
  std::string errMsg_;
  std::atomic<int> callCount_{0};
};

/// Succeeds on first call, recording the modelId it was asked to use.
class SuccessProvider : public IProvider {
public:
  explicit SuccessProvider(std::string id, std::string responseText = "rotated ok")
      : id_(std::move(id)), responseText_(std::move(responseText)) {}

  std::string getId() const override { return id_; }

  void stream(const AgentHistory &, const ProviderOptions &opts,
              std::function<void(const StreamEvent &)> onEvent) override {
    callCount_.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(mu_);
      lastModelId_ = opts.modelId;
    }
    onEvent(TextChunk{responseText_});
    onEvent(StreamDone{StopReason::Stop});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo m;
    m.id = "success-model";
    m.provider = id_;
    m.contextWindow = 4096;
    return {m};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }

  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> * = nullptr) override {
    onEvent(TextChunk{"summary"});
    onEvent(StreamDone{StopReason::Stop});
  }

  ProviderType getProviderType() const override {
    return ProviderType::APIKey;
  }

  int callCount() const { return callCount_.load(); }

  std::string lastModelId() const {
    std::lock_guard<std::mutex> lock(mu_);
    return lastModelId_;
  }

private:
  std::string id_;
  std::string responseText_;
  std::atomic<int> callCount_{0};
  mutable std::mutex mu_;
  std::string lastModelId_;
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class ModelRotationTest : public ::testing::Test {
protected:
  void SetUp() override {
    Panic::init();
    originalHome_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    originalPromptsDir_ = std::getenv("FIRMIUS_PROMPTS_DIR")
                              ? std::getenv("FIRMIUS_PROMPTS_DIR")
                              : "";
    originalConfig_ = ConfigLoader::instance().getConfig();

    testHome_ =
        std::filesystem::temp_directory_path() /
        ("firmius_model_rotation_test_" +
         std::to_string(static_cast<long long>(
             std::chrono::steady_clock::now().time_since_epoch().count())));
    testPromptsDir_ = testHome_ / "prompts";
    std::filesystem::create_directories(testHome_ / ".firmius" / "threads");
    std::filesystem::create_directories(testPromptsDir_);
    setenv("HOME", testHome_.c_str(), 1);
    setenv("FIRMIUS_PROMPTS_DIR", testPromptsDir_.c_str(), 1);

    std::ofstream coderFile(testPromptsDir_ / "coder.md");
    coderFile << "---\nname: coder\ntitle: Coder\n---\nCoder identity";
    coderFile.close();
  }

  void TearDown() override {
    Engine::instance().shutdown();
    for (const auto &agentId : AgentRegistry::instance().listAll()) {
      AgentRegistry::instance().unregisterAgent(agentId);
    }
    ConfigLoader::instance().updateConfig(originalConfig_);
    // Clear any preferred model keys left over from tests
    ConfigLoader::instance().clearPreferredModelKey("test-cat");
    std::filesystem::remove_all(testHome_);
    if (originalHome_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", originalHome_.c_str(), 1);
    }
    if (originalPromptsDir_.empty()) {
      unsetenv("FIRMIUS_PROMPTS_DIR");
    } else {
      setenv("FIRMIUS_PROMPTS_DIR", originalPromptsDir_.c_str(), 1);
    }
  }

  std::string createThread() {
    ThreadMetadata metadata;
    metadata.title = "Model Rotation Test";
    metadata.cwd = testHome_.string();
    metadata.hostOptions.type = HostType::Local;
    metadata.leadPersona = "lead";
    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    return tm.createThread(metadata);
  }

  /// Set up a category "test-cat" with failProvider first, successProvider second.
  /// Also set purpose route so persona "coder" maps to "test-cat".
  void configureRotationCategory(
      const std::shared_ptr<FailingProvider> &failProv,
      const std::shared_ptr<SuccessProvider> &successProv) {
    auto cfg = ConfigLoader::instance().getConfig();

    // Default provider points to the failing provider so the agent
    // initially picks it from the category.
    cfg.defaultProviderId = failProv->getId();
    cfg.defaultModelId = "fail-model";

    ModelOption opt1;
    opt1.providerId = failProv->getId();
    opt1.modelId = "fail-model";

    ModelOption opt2;
    opt2.providerId = successProv->getId();
    opt2.modelId = "success-model";

    ModelRouteCategory cat;
    cat.models = {opt1, opt2};

    cfg.modelRouterCategories["test-cat"] = cat;
    cfg.purposeRoutes["coder"] = "test-cat";
    cfg.defaultRouteCategory = "test-cat";

    ConfigLoader::instance().updateConfig(cfg);
  }

  static std::optional<AgentOutcome>
  waitForOutcome(const std::string &agentId,
                 std::chrono::seconds timeout = std::chrono::seconds(15)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto outcome = Engine::instance().waitForAgentOutcome(
          agentId, std::chrono::milliseconds(50));
      if (outcome.has_value()) {
        return outcome;
      }
    }
    return std::nullopt;
  }

  std::filesystem::path testHome_;
  std::filesystem::path testPromptsDir_;
  std::string originalHome_;
  std::string originalPromptsDir_;
  UserConfig originalConfig_;
  static inline std::atomic<int> providerSerial_{0};
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/// Reproducer: first model in category fails with non-retryable error (401).
/// The agent should rotate to the next model and complete successfully.
TEST_F(ModelRotationTest, RotatesToNextModelOnNonRetryableError) {
  const std::string failId =
      "fail-prov-" + std::to_string(providerSerial_.fetch_add(1));
  const std::string successId =
      "success-prov-" + std::to_string(providerSerial_.fetch_add(1));

  auto failProv = std::make_shared<FailingProvider>(failId, 401, "Unauthorized");
  auto successProv = std::make_shared<SuccessProvider>(successId, "rotation success");

  ProviderRegistry::instance().registerProvider(failProv);
  ProviderRegistry::instance().registerProvider(successProv);
  configureRotationCategory(failProv, successProv);

  const std::string threadId = createThread();
  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "say hello");

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value())
      << "Agent did not produce an outcome within timeout";
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Response)
      << "Expected successful response after rotation, got kind="
      << static_cast<int>(outcome->kind) << " text=" << outcome->text;
  EXPECT_EQ(outcome->text, "rotation success");

  // The failing provider should have been called exactly once (the initial
  // attempt that triggered rotation).
  EXPECT_GE(failProv->callCount(), 1);

  // The success provider should have been called at least once.
  EXPECT_GE(successProv->callCount(), 1);
}

/// After rotation, the preferred model key in ConfigLoader should reflect
/// the rotated model.
TEST_F(ModelRotationTest, PreferredModelKeyIsSetAfterRotation) {
  const std::string failId =
      "fail-prov-pref-" + std::to_string(providerSerial_.fetch_add(1));
  const std::string successId =
      "success-prov-pref-" + std::to_string(providerSerial_.fetch_add(1));

  auto failProv = std::make_shared<FailingProvider>(failId, 401, "Unauthorized");
  auto successProv = std::make_shared<SuccessProvider>(successId, "preferred ok");

  ProviderRegistry::instance().registerProvider(failProv);
  ProviderRegistry::instance().registerProvider(successProv);
  configureRotationCategory(failProv, successProv);

  const std::string threadId = createThread();
  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "check key");

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Response);

  // After rotation, the preferred key for "test-cat" should point to the
  // success provider.
  const std::string preferredKey =
      ConfigLoader::instance().getPreferredModelKey("test-cat");
  const std::string expectedKey = successId + ":success-model";
  EXPECT_EQ(preferredKey, expectedKey);
}

/// If the category has only one model and it fails, rotation should NOT happen
/// and the agent should fail with an error outcome.
TEST_F(ModelRotationTest, SingleModelCategoryDoesNotRotate) {
  const std::string failId =
      "fail-prov-single-" + std::to_string(providerSerial_.fetch_add(1));

  auto failProv = std::make_shared<FailingProvider>(failId, 401, "Unauthorized");
  ProviderRegistry::instance().registerProvider(failProv);

  auto cfg = ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = failProv->getId();
  cfg.defaultModelId = "fail-model";

  ModelOption opt;
  opt.providerId = failProv->getId();
  opt.modelId = "fail-model";

  ModelRouteCategory cat;
  cat.models = {opt};

  cfg.modelRouterCategories["test-cat"] = cat;
  cfg.purposeRoutes["coder"] = "test-cat";
  cfg.defaultRouteCategory = "test-cat";
  ConfigLoader::instance().updateConfig(cfg);

  const std::string threadId = createThread();
  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "fail please");

  auto outcome = waitForOutcome(agentId);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Failed)
      << "Expected failure (no rotation possible), text=" << outcome->text;
}

/// Rotation after exhausting retries on retryable errors (e.g. 500).
/// The agent should retry up to maxProviderRetries, then rotate.
TEST_F(ModelRotationTest, RotatesAfterExhaustingRetriesOnRetryableError) {
  const std::string failId =
      "fail-prov-retry-" + std::to_string(providerSerial_.fetch_add(1));
  const std::string successId =
      "success-prov-retry-" + std::to_string(providerSerial_.fetch_add(1));

  // 500 is retryable, so the agent will retry 3 times before rotating
  auto failProv = std::make_shared<FailingProvider>(failId, 500, "Internal Server Error");
  auto successProv = std::make_shared<SuccessProvider>(successId, "retry rotation ok");

  ProviderRegistry::instance().registerProvider(failProv);
  ProviderRegistry::instance().registerProvider(successProv);
  configureRotationCategory(failProv, successProv);

  const std::string threadId = createThread();
  const std::string agentId =
      Engine::instance().summonAgent(threadId, "coder", "retry test");

  // This test takes longer because of retry delays (1+2+4 = 7 seconds exponential backoff)
  // but the interruptible sleep mechanism should make actual waits shorter in test
  auto outcome = waitForOutcome(agentId, std::chrono::seconds(30));
  ASSERT_TRUE(outcome.has_value())
      << "Agent did not produce outcome after retryable rotation";
  EXPECT_EQ(outcome->kind, AgentOutcome::Kind::Response)
      << "Expected response after retry-exhaustion rotation, text=" << outcome->text;
  EXPECT_EQ(outcome->text, "retry rotation ok");

  // The failing provider should have been called 1 (initial) + 3 (retries) = 4 times
  EXPECT_GE(failProv->callCount(), 4);

  // The success provider should have been called at least once
  EXPECT_GE(successProv->callCount(), 1);
}

} // namespace
