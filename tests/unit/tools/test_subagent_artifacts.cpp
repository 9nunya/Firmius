#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "Engine.hpp"
#include "IAgent.hpp"
#include "IHost.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include "tools/SubagentTool.hpp"
#include "tools/SubagentWaitTool.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <thread>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;
using ::testing::NiceMock;
using ::testing::ReturnRef;

namespace {

class MockHost : public IHost {
public:
  MOCK_METHOD(std::string, init, (), (override));
  MOCK_METHOD(void, destroy, (), (override));
  MOCK_METHOD(void, cleanup, (), (override));
  MOCK_METHOD(void, setUser, (const std::string &), (override));
  MOCK_METHOD(std::vector<uint8_t>, readFile, (const std::string &), (override));
  MOCK_METHOD(void, writeFile,
              (const std::string &, (const std::vector<uint8_t> &)), (override));
  MOCK_METHOD(bool, exists, (const std::string &), (override));
  MOCK_METHOD(std::vector<FileInfo>, listDir, (const std::string &), (override));
  MOCK_METHOD(FileInfo, stat, (const std::string &), (override));
  MOCK_METHOD(std::string, getId, (), (const, override));
  MOCK_METHOD((ProcessResult), exec,
              (const std::string &, const std::string &,
               (const std::map<std::string, std::string> &),
               std::optional<std::chrono::milliseconds>),
              (override));
  MOCK_METHOD((std::unique_ptr<IHostProcess>), spawn,
              (const std::string &, const std::string &,
               (const std::map<std::string, std::string> &)),
              (override));
  MOCK_METHOD(void, registerBackgroundProcess,
              (const std::string &, (std::unique_ptr<IHostProcess>)), (override));
  MOCK_METHOD(ProcessSnapshot, inspectBackgroundProcess, (const std::string &),
              (override));
  MOCK_METHOD(void, writeToBackgroundProcess,
              (const std::string &, const std::string &), (override));
  MOCK_METHOD(void, killBackgroundProcess, (const std::string &), (override));
};

class MockAgent : public IAgent {
public:
  AgentContext defaultCtx;

  MockAgent() { defaultCtx.history = std::make_shared<AgentHistory>(); }

  std::shared_ptr<IEnvironment> getEnvironment() const override {
    return nullptr;
  }
  std::shared_ptr<IPermissions> getPermissions() const override {
    return nullptr;
  }

  MOCK_METHOD(void, reset, (), (override));
  MOCK_METHOD(void, run,
              (const std::string &, (std::function<void(const StreamEvent &)>),
               const std::vector<ImageContent> &),
              (override));
  MOCK_METHOD(void, resume, ((std::function<void(const StreamEvent &)>)),
              (override));
  MOCK_METHOD((const AgentContext &), getContext, (), (const, override));
  MOCK_METHOD(AgentContext &, getMutableContext, (), (override));
  MOCK_METHOD(ModelChoice, getPreferredModel, (), (const, override));
  MOCK_METHOD(void, interrupt, (), (override));
  MOCK_METHOD(bool, isInterrupted, (), (const, override));
  MOCK_METHOD(void, clearInterrupt, (), (override));
  MOCK_METHOD(void, compactNow, (std::function<void(const StreamEvent &)>),
              (override));
  MOCK_METHOD(void, saveHistory, (), (override));
  MOCK_METHOD(void, setModel, (const std::string &, const std::string &),
              (override));
  MOCK_METHOD(void, setModel,
              (const std::string &, const std::string &, const std::string &),
              (override));
  MOCK_METHOD(bool, isRunning, (), (const, override));
  MOCK_METHOD(bool, isBooting, (), (const, override));
  MOCK_METHOD(void, setBooting, (bool), (override));
  MOCK_METHOD((std::shared_ptr<IHost>), getHost, (), (override));
};

class ArtifactToolProvider : public IProvider {
public:
  ArtifactToolProvider(std::string providerId, bool emitSummary)
      : providerId_(std::move(providerId)), emitSummary_(emitSummary) {}

  std::string getId() const override { return providerId_; }

  void stream(const AgentHistory &, const ProviderOptions &,
              std::function<void(const StreamEvent &)> onEvent) override {
    const int n = callCount_.fetch_add(1);
    if (n == 0) {
      onEvent(ToolCallChunk{"artifact-write-1", 0, "artifact_write",
                            R"({"name":"WORKER_REPORT.md","content":"artifact-body"})"});
      onEvent(StreamDone{StopReason::ToolUse});
      return;
    }
    if (emitSummary_) {
      onEvent(TextChunk{"child-summary"});
    }
    onEvent(StreamDone{StopReason::Stop});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = providerId_ + "-model";
    model.provider = providerId_;
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }

  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> * = nullptr) override {
    if (emitSummary_) {
      onEvent(TextChunk{"child-summary"});
    }
    onEvent(StreamDone{StopReason::Stop});
  }

  ProviderType getProviderType() const override { return ProviderType::APIKey; }

  int callCount() const { return callCount_.load(); }

private:
  std::string providerId_;
  bool emitSummary_;
  std::atomic<int> callCount_{0};
};

class CancelOnlyProvider : public IProvider {
public:
  explicit CancelOnlyProvider(std::string providerId)
      : providerId_(std::move(providerId)) {}

  std::string getId() const override { return providerId_; }

  void stream(const AgentHistory &, const ProviderOptions &opts,
              std::function<void(const StreamEvent &)> onEvent) override {
    entered_.set_value();
    while (!opts.abortSignal || !opts.abortSignal->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    onEvent(StreamDone{StopReason::Stop});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = providerId_ + "-model";
    model.provider = providerId_;
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }

  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> * = nullptr) override {
    onEvent(StreamDone{StopReason::Stop});
  }

  ProviderType getProviderType() const override { return ProviderType::APIKey; }

  void waitUntilEntered() { entered_.get_future().wait(); }

private:
  std::string providerId_;
  std::promise<void> entered_;
};

rapidjson::Document parseJson(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  return doc;
}

class SubagentArtifactMetadataTest : public ::testing::Test {
protected:
  void SetUp() override {
    originalHome_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    originalPromptsDir_ =
        std::getenv("FIRMIUS_PROMPTS_DIR") ? std::getenv("FIRMIUS_PROMPTS_DIR")
                                            : "";

    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_subagent_artifacts_" +
                 std::to_string(static_cast<long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())));
    promptsDir_ = testHome_ / "prompts";
    std::filesystem::create_directories(testHome_ / ".firmius" / "threads");
    std::filesystem::create_directories(promptsDir_);
    setenv("HOME", testHome_.c_str(), 1);
    setenv("FIRMIUS_PROMPTS_DIR", promptsDir_.c_str(), 1);

    {
      std::ofstream base(promptsDir_ / "base.md");
      base << "base";
    }
    {
      std::ofstream lead(promptsDir_ / "lead.md");
      lead << "---\nname: lead\ntitle: Lead\nscopes: [\"Delegation\"]\n---\nLead";
    }
    {
      std::ofstream coder(promptsDir_ / "coder.md");
      coder << "---\nname: coder\ntitle: Coder\nscopes: [\"Semantic\"]\n---\nCoder";
    }

    ThreadManager tm((testHome_ / ".firmius" / "threads").string());
    ThreadMetadata metadata;
    metadata.title = "Subagent Artifact Metadata Test";
    metadata.cwd = testHome_.string();
    metadata.hostOptions.type = HostType::Local;
    metadata.leadPersona = "lead";
    threadId_ = tm.createThread(metadata);

    parent_.defaultCtx.history->threadId = threadId_;
    parent_.defaultCtx.identity.id = "parent-agent";
    parent_.defaultCtx.identity.friendlyName = "parent";
    parent_.defaultCtx.environment.cwd = testHome_.string();
    ON_CALL(parent_, getContext()).WillByDefault(ReturnRef(parent_.defaultCtx));
    ON_CALL(parent_, getMutableContext())
        .WillByDefault(ReturnRef(parent_.defaultCtx));
  }

  void TearDown() override {
    Engine::instance().shutdown();
    for (const auto &id : AgentRegistry::instance().listAll()) {
      AgentRegistry::instance().unregisterAgent(id);
    }

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

  void configureDefaultProvider(const std::shared_ptr<IProvider> &provider) {
    ProviderRegistry::instance().registerProvider(provider);
    auto cfg = ConfigLoader::instance().getConfig();
    cfg.defaultProviderId = provider->getId();
    cfg.defaultModelId = provider->listModels().front().id;
    ConfigLoader::instance().updateConfig(cfg);
  }

  std::filesystem::path testHome_;
  std::filesystem::path promptsDir_;
  std::string originalHome_;
  std::string originalPromptsDir_;
  std::string threadId_;
  NiceMock<MockHost> host_;
  NiceMock<MockAgent> parent_;
};

TEST_F(SubagentArtifactMetadataTest, SummonSubagentSyncReturnsArtifactsCreated) {
  auto provider = std::make_shared<ArtifactToolProvider>(
      "subagent-artifact-provider-sync", true);
  configureDefaultProvider(provider);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Write a report artifact.";
  input.name = "child-sync";
  input.title = "Child Sync";
  input.async = false;

  ToolContext ctx{host_, parent_, "subagent-artifacts-sync"};
  ToolResult result = tool.execute(input, ctx);
  ASSERT_TRUE(result.success) << result.error;

  rapidjson::Document json = parseJson(result.data);
  ASSERT_FALSE(json.HasParseError());
  EXPECT_EQ(std::string(json["status"].GetString()), "completed");
  ASSERT_TRUE(json.HasMember("artifacts_created"));
  ASSERT_TRUE(json["artifacts_created"].IsArray());
  ASSERT_EQ(json["artifacts_created"].Size(), 1u);
  EXPECT_EQ(std::string(json["artifacts_created"][0]["filename"].GetString()),
            "WORKER_REPORT.md");
  ASSERT_TRUE(json.HasMember("artifacts_updated"));
  ASSERT_TRUE(json["artifacts_updated"].IsArray());
  EXPECT_EQ(json["artifacts_updated"].Size(), 0u);
}

TEST_F(SubagentArtifactMetadataTest,
       SubagentWaitReportsArtifactsEvenWhenChildHasNoSummary) {
  auto provider = std::make_shared<ArtifactToolProvider>(
      "subagent-artifact-provider-no-summary", false);
  configureDefaultProvider(provider);

  SubagentTool summonTool;
  SubagentInput summon;
  summon.persona = "coder";
  summon.task = "Write a report artifact.";
  summon.name = "child-async";
  summon.title = "Child Async";
  summon.async = true;

  ToolContext summonCtx{host_, parent_, "subagent-artifacts-async"};
  ToolResult summonResult = summonTool.execute(summon, summonCtx);
  ASSERT_TRUE(summonResult.success) << summonResult.error;
  rapidjson::Document summonJson = parseJson(summonResult.data);
  ASSERT_FALSE(summonJson.HasParseError());
  const std::string agentId = summonJson["agentId"].GetString();

  SubagentWaitTool waitTool;
  SubagentWaitInput waitInput;
  waitInput.agent_id = agentId;
  ToolContext waitCtx{host_, parent_, "subagent-wait-no-summary"};
  ToolResult waitResult = waitTool.execute(waitInput, waitCtx);
  ASSERT_TRUE(waitResult.success) << waitResult.error;

  rapidjson::Document waitJson = parseJson(waitResult.data);
  ASSERT_FALSE(waitJson.HasParseError());
  EXPECT_EQ(std::string(waitJson["status"].GetString()), "completed_no_summary");
  ASSERT_TRUE(waitJson.HasMember("artifacts_created"));
  ASSERT_TRUE(waitJson["artifacts_created"].IsArray());
  ASSERT_EQ(waitJson["artifacts_created"].Size(), 1u);
}

TEST_F(SubagentArtifactMetadataTest,
       CancelledChildDoesNotReportSpuriousArtifacts) {
  auto provider = std::make_shared<CancelOnlyProvider>("subagent-cancel-provider");
  configureDefaultProvider(provider);

  SubagentTool summonTool;
  SubagentInput summon;
  summon.persona = "coder";
  summon.task = "Wait for cancellation.";
  summon.name = "child-cancel";
  summon.title = "Child Cancel";
  summon.async = true;

  ToolContext summonCtx{host_, parent_, "subagent-artifacts-cancel-summon"};
  ToolResult summonResult = summonTool.execute(summon, summonCtx);
  ASSERT_TRUE(summonResult.success) << summonResult.error;
  rapidjson::Document summonJson = parseJson(summonResult.data);
  ASSERT_FALSE(summonJson.HasParseError());
  const std::string agentId = summonJson["agentId"].GetString();

  provider->waitUntilEntered();
  Engine::instance().cancelAgent(agentId);

  SubagentWaitTool waitTool;
  SubagentWaitInput waitInput;
  waitInput.agent_id = agentId;
  ToolContext waitCtx{host_, parent_, "subagent-artifacts-cancel-wait"};
  ToolResult waitResult = waitTool.execute(waitInput, waitCtx);
  ASSERT_TRUE(waitResult.success) << waitResult.error;

  rapidjson::Document waitJson = parseJson(waitResult.data);
  ASSERT_FALSE(waitJson.HasParseError());
  EXPECT_EQ(std::string(waitJson["status"].GetString()), "cancelled");
  ASSERT_TRUE(waitJson.HasMember("artifacts_created"));
  ASSERT_TRUE(waitJson["artifacts_created"].IsArray());
  EXPECT_EQ(waitJson["artifacts_created"].Size(), 0u);
  ASSERT_TRUE(waitJson.HasMember("artifacts_updated"));
  ASSERT_TRUE(waitJson["artifacts_updated"].IsArray());
  EXPECT_EQ(waitJson["artifacts_updated"].Size(), 0u);
}

TEST_F(SubagentArtifactMetadataTest,
       MissingReferenceFailsBeforeDispatchingChildAgent) {
  auto provider = std::make_shared<ArtifactToolProvider>(
      "subagent-artifact-provider-no-dispatch", true);
  configureDefaultProvider(provider);

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Review @src/does-not-exist.ts before writing.";
  input.name = "child-no-dispatch";
  input.title = "Child No Dispatch";
  input.async = true;

  ToolContext ctx{host_, parent_, "subagent-artifacts-reference-fail"};
  ToolResult result = tool.execute(input, ctx);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, ::testing::HasSubstr("Reference expansion failed"));
  EXPECT_EQ(provider->callCount(), 0);
}

TEST_F(SubagentArtifactMetadataTest,
       TrailingArtifactPunctuationStillDispatchesChildAgentSafely) {
  auto provider = std::make_shared<ArtifactToolProvider>(
      "subagent-artifact-provider-punctuation", true);
  configureDefaultProvider(provider);

  ThreadManager tm((testHome_ / ".firmius" / "threads").string());
  tm.writeArtifact(threadId_, "planner-agent", "planner", "REPORT.md", "alpha");

  SubagentTool tool;
  SubagentInput input;
  input.persona = "coder";
  input.task = "Review (@artifact:REPORT.md). Then write a report.";
  input.name = "child-punctuation";
  input.title = "Child Punctuation";
  input.async = false;

  ToolContext ctx{host_, parent_, "subagent-artifacts-reference-punctuation"};
  ToolResult result = tool.execute(input, ctx);
  ASSERT_TRUE(result.success) << result.error;
}

} // namespace
