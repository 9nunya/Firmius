#include "IAgent.hpp"
#include "IHost.hpp"
#include "persistence/ThreadManager.hpp"
#include "tools/ArtifactsTool.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rapidjson/document.h>

using namespace firmius::core;
using namespace firmius::shared;
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
  MOCK_METHOD(void, releaseBackgroundProcess, (const std::string &),
              (override));
  MOCK_METHOD(void, writeToBackgroundProcess,
              (const std::string &, const std::string &), (override));
  MOCK_METHOD(void, killBackgroundProcess, (const std::string &), (override));
};

class MockAgent : public IAgent {
public:
  AgentContext defaultCtx;

  MockAgent() {
    defaultCtx.history = std::make_shared<AgentHistory>();
  }

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
  MOCK_METHOD(void, appendHistoryTurn, (const AgentTurn &), (override));
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

rapidjson::Document objectDoc() {
  rapidjson::Document doc;
  doc.SetObject();
  return doc;
}

class ArtifactToolsTest : public ::testing::Test {
protected:
  void SetUp() override {
    originalHome_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    testHome_ = std::filesystem::temp_directory_path() /
                ("firmius_artifact_tools_" +
                 std::to_string(static_cast<long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(testHome_ / ".firmius" / "threads");
    setenv("HOME", testHome_.c_str(), 1);

    manager_ = std::make_unique<ThreadManager>(
        (testHome_ / ".firmius" / "threads").string());

    ThreadMetadata metadata;
    metadata.title = "Artifact Tools Test";
    metadata.cwd = testHome_.string();
    metadata.hostOptions.type = HostType::Local;
    metadata.leadPersona = "lead";
    threadId_ = manager_->createThread(metadata);

    manager_->writeAgentManifest(
        threadId_,
        {{"agent-1", {"planner", "", "planner", "Planner", true}},
         {"agent-2", {"auditor", "", "auditor", "Auditor", true}}});

    parent_.defaultCtx.history->threadId = threadId_;
    parent_.defaultCtx.identity.id = "agent-1";
    parent_.defaultCtx.identity.friendlyName = "planner";
    ON_CALL(parent_, getContext()).WillByDefault(ReturnRef(parent_.defaultCtx));
    ON_CALL(parent_, getMutableContext())
        .WillByDefault(ReturnRef(parent_.defaultCtx));
  }

  void TearDown() override {
    manager_.reset();
    std::filesystem::remove_all(testHome_);
    if (originalHome_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", originalHome_.c_str(), 1);
    }
  }

  std::filesystem::path testHome_;
  std::string originalHome_;
  std::unique_ptr<ThreadManager> manager_;
  std::string threadId_;
  NiceMock<MockAgent> parent_;
  NiceMock<MockHost> host_;
};

TEST_F(ArtifactToolsTest, WriteCreateUpdateAndReadRoundTrip) {
  ArtifactsTool tool;
  ToolContext ctx{host_, parent_, "artifact-tools-create-update"};

  rapidjson::Document createDoc;
  createDoc.SetObject();
  auto &ca = createDoc.GetAllocator();
  createDoc.AddMember("action", rapidjson::Value("Write", ca), ca);
  createDoc.AddMember("name", rapidjson::Value("REPORT.md", ca), ca);
  createDoc.AddMember("content", rapidjson::Value("first body", ca), ca);
  createDoc.AddMember("kind", rapidjson::Value("report", ca), ca);
  createDoc.AddMember("description", rapidjson::Value("initial", ca), ca);
  ToolResult created = tool.execute(createDoc, ctx);
  ASSERT_TRUE(created.success) << created.error;

  rapidjson::Document createdJson;
  createdJson.Parse(created.data.c_str());
  ASSERT_FALSE(createdJson.HasParseError());
  EXPECT_TRUE(createdJson["created"].GetBool());
  EXPECT_EQ(std::string(createdJson["status"].GetString()), "created");
  EXPECT_EQ(std::string(createdJson["reference"].GetString()),
            "@artifact:planner/REPORT.md");

  rapidjson::Document updateDoc;
  updateDoc.SetObject();
  auto &ua = updateDoc.GetAllocator();
  updateDoc.AddMember("action", rapidjson::Value("Write", ua), ua);
  updateDoc.AddMember("name", rapidjson::Value("REPORT.md", ua), ua);
  updateDoc.AddMember("content", rapidjson::Value("second body", ua), ua);
  ToolResult updated = tool.execute(updateDoc, ctx);
  ASSERT_TRUE(updated.success) << updated.error;
  rapidjson::Document updatedJson;
  updatedJson.Parse(updated.data.c_str());
  ASSERT_FALSE(updatedJson.HasParseError());
  EXPECT_TRUE(updatedJson["updated"].GetBool());
  EXPECT_EQ(std::string(updatedJson["status"].GetString()), "updated");
  ASSERT_TRUE(updatedJson.HasMember("previous_content"));
  EXPECT_EQ(std::string(updatedJson["previous_content"].GetString()),
            "first body");

  rapidjson::Document readDoc;
  readDoc.SetObject();
  auto &ra = readDoc.GetAllocator();
  readDoc.AddMember("action", rapidjson::Value("Read", ra), ra);
  readDoc.AddMember("reference", rapidjson::Value("@artifact:planner/REPORT.md", ra), ra);
  ToolResult read = tool.execute(readDoc, ctx);
  ASSERT_TRUE(read.success) << read.error;
  rapidjson::Document readJson;
  readJson.Parse(read.data.c_str());
  ASSERT_FALSE(readJson.HasParseError());
  EXPECT_EQ(std::string(readJson["content"].GetString()), "second body");
}

TEST_F(ArtifactToolsTest, ListIncludesDisambiguatedDisplaysForDuplicateFilenames) {
  manager_->writeArtifact(threadId_, "agent-1", "planner", "REPORT.md",
                          "planner-body");
  manager_->writeArtifact(threadId_, "agent-2", "auditor", "REPORT.md",
                          "auditor-body");

  ArtifactsTool listTool;
  ToolContext ctx{host_, parent_, "artifact-tools-list"};
  rapidjson::Document doc = objectDoc(); auto &a = doc.GetAllocator(); doc.AddMember("action", rapidjson::Value("List", a), a);
  ToolResult listed = listTool.execute(doc, ctx);
  ASSERT_TRUE(listed.success) << listed.error;

  rapidjson::Document listedJson;
  listedJson.Parse(listed.data.c_str());
  ASSERT_FALSE(listedJson.HasParseError());
  ASSERT_TRUE(listedJson.HasMember("artifacts"));
  ASSERT_TRUE(listedJson["artifacts"].IsArray());
  ASSERT_EQ(listedJson["artifacts"].Size(), 2u);

  const auto &first = listedJson["artifacts"][0];
  const auto &second = listedJson["artifacts"][1];
  EXPECT_TRUE(first["ambiguous_filename"].GetBool());
  EXPECT_TRUE(second["ambiguous_filename"].GetBool());
  EXPECT_NE(std::string(first["display"].GetString()),
            std::string(second["display"].GetString()));
  EXPECT_NE(std::string(first["reference"].GetString()).find("@artifact:"),
            std::string::npos);
}

TEST_F(ArtifactToolsTest, ReadFailsForAmbiguousFilenameWithoutOwnerSelector) {
  manager_->writeArtifact(threadId_, "agent-1", "planner", "REPORT.md", "A");
  manager_->writeArtifact(threadId_, "agent-2", "auditor", "REPORT.md", "B");

  ArtifactsTool readTool;
  ToolContext ctx{host_, parent_, "artifact-tools-read-ambiguous"};
  rapidjson::Document input; input.SetObject(); auto &a = input.GetAllocator(); input.AddMember("action", rapidjson::Value("Read", a), a); input.AddMember("name", rapidjson::Value("REPORT.md", a), a);

  ToolResult result = readTool.execute(input, ctx);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, ::testing::HasSubstr("ambiguous"));
}

} // namespace
