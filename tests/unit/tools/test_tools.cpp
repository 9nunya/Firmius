#include "Events.hpp"
#include "IAgent.hpp"
#include "ITool.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "hosts/LocalHost.hpp"
#include "tools/FileReadTool.hpp"
#include "tools/FileEditTool.hpp"
#include "tools/GlobTool.hpp"
#include "tools/GrepTool.hpp"
#include "tools/ListDirectoryTool.hpp"
#include "tools/ProcessExecuteTool.hpp"
#include "tools/ProcessSpawnTool.hpp"
#include "tools/PythonExecuteTool.hpp"
#include <filesystem>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <set>

using namespace firmius::core;
using namespace firmius::shared;
using ::testing::_;
using ::testing::HasSubstr;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

class MockHost : public IHost {
public:
  MOCK_METHOD(std::string, init, (), (override));
  MOCK_METHOD(void, destroy, (), (override));
  MOCK_METHOD(void, cleanup, (), (override));
  MOCK_METHOD(void, setUser, (const std::string &), (override));
  MOCK_METHOD(std::vector<uint8_t>, readFile, (const std::string &),
              (override));
  MOCK_METHOD(void, writeFile,
              (const std::string &, (const std::vector<uint8_t> &)),
              (override));
  MOCK_METHOD(bool, exists, (const std::string &), (override));
  MOCK_METHOD(std::vector<FileInfo>, listDir, (const std::string &),
              (override));
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
              (const std::string &, (std::unique_ptr<IHostProcess>)),
              (override));
  MOCK_METHOD(ProcessSnapshot, inspectBackgroundProcess, (const std::string &),
              (override));
  MOCK_METHOD(void, writeToBackgroundProcess,
              (const std::string &, const std::string &), (override));
  MOCK_METHOD(void, killBackgroundProcess, (const std::string &), (override));
};

#include "../mocks/MockEnvironment.hpp"

// Local MockAgent that uses MOCK_METHOD for all interface methods
class MockAgent : public IAgent {
public:
  firmius::shared::AgentContext defaultCtx;
  std::shared_ptr<firmius::test::MockEnvironment> mockEnv_;
  std::shared_ptr<firmius::test::MockPermissions> mockPerms_;

  MockAgent() 
    : mockEnv_(std::make_shared<firmius::test::MockEnvironment>())
    , mockPerms_(std::make_shared<firmius::test::MockPermissions>()) {
    if (!defaultCtx.history) {
      defaultCtx.history = std::make_shared<AgentHistory>();
    }
  }

  std::shared_ptr<IEnvironment> getEnvironment() const override { return mockEnv_; }
  std::shared_ptr<IPermissions> getPermissions() const override { return mockPerms_; }

  MOCK_METHOD(void, reset, (), (override));
  MOCK_METHOD(void, run,
              (const std::string &, (std::function<void(const StreamEvent &)>),
               const std::vector<ImageContent> &),
              (override));
  MOCK_METHOD((const AgentContext &), getContext, (), (const, override));
  MOCK_METHOD(AgentContext &, getMutableContext, (), (override));
  MOCK_METHOD(void, interrupt, (), (override));
  MOCK_METHOD(bool, isInterrupted, (), (const, override));
  MOCK_METHOD(void, clearInterrupt, (), (override));
  MOCK_METHOD(void, compactNow,
              (std::function<void(const StreamEvent &)>), (override));
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

class MockHostProcess : public IHostProcess {
public:
  MOCK_METHOD(void, onOutput,
              ((std::function<void(const std::string &, bool)>)), (override));
  MOCK_METHOD(ProcessResult, wait, (), (override));
  MOCK_METHOD(ProcessSnapshot, inspect, (), (const, override));
  MOCK_METHOD(void, kill, (), (override));
  MOCK_METHOD(void, write, (const std::string &), (override));
  MOCK_METHOD(bool, isRunning, (), (override));
  MOCK_METHOD(std::string, getSystemId, (), (const, override));
};

rapidjson::Value makeJsonString(const std::string &str,
                                rapidjson::Document::AllocatorType &alloc) {
  return rapidjson::Value(str.c_str(), alloc);
}

rapidjson::Document
createJsonInput(const std::map<std::string, std::string> &stringFields,
                const std::map<std::string, int> &intFields = {}) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();

  for (const auto &[key, value] : stringFields) {
    doc.AddMember(makeJsonString(key, alloc), makeJsonString(value, alloc),
                  alloc);
  }

  for (const auto &[key, value] : intFields) {
    doc.AddMember(makeJsonString(key, alloc), rapidjson::Value(value), alloc);
  }

  return doc;
}

class FileReadToolTest : public ::testing::Test {
protected:
  FileReadTool tool;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  AgentContext agentContext;

  void SetUp() override {
    mockAgent.defaultCtx.environment.cwd = "/tmp/work";
    mockAgent.defaultCtx.permissions.allowOutsideCwd = false;
    mockAgent.defaultCtx.permissions.allowedPaths = {"/tmp/work", "/tmp"};

    mockAgent.mockPerms_->allowOutsideCwd_ = false;
    mockAgent.mockPerms_->allowedPaths_ = {"/tmp/work", "/tmp"};
    mockAgent.mockPerms_->cwd_ = "/tmp/work";

    ON_CALL(mockAgent, getContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent, getMutableContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), resolvePath(_))
        .WillByDefault(Invoke([](const std::string &path) {
          if (path.starts_with("/"))
            return path;
          return "/tmp/work/" + path;
        }));
    ON_CALL(mockHost, readFile(_))
        .WillByDefault(Invoke([](const std::string &path) {
          std::ifstream f(path, std::ios::binary);
          if (!f)
            throw std::runtime_error("Cannot read: " + path);
          return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
        }));
  }
};

TEST_F(FileReadToolTest, allowedPaths_permitsInside) {
  std::string content = "Line 1\nLine 2\nLine 3\n";
  std::filesystem::create_directories("/tmp/work");
  std::ofstream out("/tmp/work/file.txt");
  out << content;
  out.close();

  auto json = createJsonInput({{"path", "file.txt"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("Line 1"), std::string::npos);
}

TEST_F(FileReadToolTest, allowedPaths_blocksOutside) {
  mockAgent.defaultCtx.permissions.allowedPaths = {};
  mockAgent.defaultCtx.permissions.allowOutsideCwd = false;

  auto json = createJsonInput({{"path", "/etc/passwd"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Access denied"), std::string::npos);
}

TEST_F(FileReadToolTest, lineSlicing) {
  std::string content = "Line 1\nLine 2\nLine 3\nLine 4\nLine 5\n";
  std::filesystem::create_directories("/tmp/work");
  std::ofstream out("/tmp/work/file.txt");
  out << content;
  out.close();

  auto json = createJsonInput({{"path", "file.txt"}},
                              {{"start_line", 2}, {"end_line", 4}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("Line 2"), std::string::npos);
  EXPECT_NE(result.data.find("Line 3"), std::string::npos);
  EXPECT_NE(result.data.find("Line 4"), std::string::npos);
  EXPECT_EQ(result.data.find("Line 1"), std::string::npos);
  EXPECT_EQ(result.data.find("Line 5"), std::string::npos);
}

class GlobToolTest : public ::testing::Test {
protected:
  GlobTool tool;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;

  void SetUp() override {
    mockAgent.defaultCtx.permissions.allowedPaths = {"/tmp", "/root"};
    ON_CALL(mockAgent, getContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), resolvePath(_))
        .WillByDefault(Invoke([](const std::string &path) { return path; }));
  }
};

TEST_F(GlobToolTest, exitCode1_noMatches) {
  ProcessResult result;
  result.exitCode = 1;
  result.stdoutData = "";

  EXPECT_CALL(mockHost, exec(_, _, _, _)).WillOnce(Return(result));

  auto json = createJsonInput({{"pattern", "*.nonexistent"}, {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_TRUE(toolResult.success);
  EXPECT_NE(toolResult.data.find("[]"), std::string::npos);
}

TEST_F(GlobToolTest, exitCode2_error) {
  ProcessResult result;
  result.exitCode = 2;
  result.stderrData = "Permission denied";

  EXPECT_CALL(mockHost, exec(_, _, _, _)).WillOnce(Return(result));

  auto json = createJsonInput({{"pattern", "*.txt"}, {"path", "/root"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_FALSE(toolResult.success);
  EXPECT_NE(toolResult.error.find("Glob failed"), std::string::npos);
}

TEST_F(GlobToolTest, patternMatching) {
  ProcessResult result;
  result.exitCode = 0;
  result.stdoutData = "/tmp/file1.txt\n/tmp/file2.txt\n";

  EXPECT_CALL(mockHost, exec(_, _, _, _)).WillOnce(Return(result));

  auto json = createJsonInput({{"pattern", "*.txt"}, {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_TRUE(toolResult.success);
  EXPECT_NE(toolResult.data.find("file1.txt"), std::string::npos);
  EXPECT_NE(toolResult.data.find("file2.txt"), std::string::npos);
}

class GrepToolTest : public ::testing::Test {
protected:
  GrepTool tool;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;

  void SetUp() override {
    mockAgent.defaultCtx.permissions.allowedPaths = {"/tmp"};
    ON_CALL(mockAgent, getContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), resolvePath(_))
        .WillByDefault(Invoke([](const std::string &path) { return path; }));
  }
};

TEST_F(GrepToolTest, binaryFilesSkipped) {
  ProcessResult result;
  result.exitCode = 1;
  result.stdoutData = "";

  EXPECT_CALL(mockHost,
              exec(HasSubstr("--binary-files=without-match"), _, _, _))
      .WillOnce(Return(result));

  auto json = createJsonInput({{"pattern", "test"}, {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  itool->execute(json, ctx);
}

TEST_F(GrepToolTest, exitCode1_noMatches) {
  ProcessResult result;
  result.exitCode = 1;
  result.stdoutData = "";

  EXPECT_CALL(mockHost, exec(_, _, _, _)).WillOnce(Return(result));

  auto json = createJsonInput(
      {{"pattern", "nonexistent_pattern_12345"}, {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_TRUE(toolResult.success);
  EXPECT_NE(toolResult.data.find("[]"), std::string::npos);
}

class CommandPermissionToolTest : public ::testing::Test {
protected:
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;

  void SetUp() override {
    mockAgent.defaultCtx.environment.cwd = "/tmp/work";
    mockAgent.defaultCtx.permissions.allowOutsideCwd = false;
    mockAgent.defaultCtx.permissions.allowedPaths = {"/tmp/work", "/tmp"};
    mockAgent.mockPerms_->allowOutsideCwd_ = false;
    mockAgent.mockPerms_->allowedPaths_ = {"/tmp/work", "/tmp"};
    mockAgent.mockPerms_->cwd_ = "/tmp/work";

    ON_CALL(mockAgent, getContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent, getMutableContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), resolvePath(_))
        .WillByDefault(Invoke([](const std::string &path) {
          if (path.starts_with("/")) {
            return path;
          }
          return "/tmp/work/" + path;
        }));
    ON_CALL(mockAgent.mockEnv_->mockProcessManager(), removeBlockingProcessId(_))
        .WillByDefault(Return());
    ON_CALL(mockAgent, isInterrupted()).WillByDefault(Return(false));
  }
};

TEST_F(CommandPermissionToolTest,
       processExecute_deniedCommandDoesNotSpawnProcess) {
  ProcessExecuteTool tool;
  mockAgent.mockPerms_->commandApprovalResponse_ = PermissionResponse::Deny;

  auto json =
      createJsonInput({{"command", "touch denied.txt"}, {"cwd", "/tmp/work"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _))
      .Times(0);

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  ASSERT_EQ(mockAgent.mockPerms_->requestedCommands_.size(), 1u);
  EXPECT_EQ(mockAgent.mockPerms_->requestedCommands_[0], "touch denied.txt");
}

TEST_F(CommandPermissionToolTest,
       processSpawn_deniedCommandDoesNotSpawnProcess) {
  ProcessSpawnTool tool;
  mockAgent.mockPerms_->commandApprovalResponse_ = PermissionResponse::Deny;

  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("command", makeJsonString("sleep 10", alloc), alloc);

  ToolContext ctx{mockHost, mockAgent, "test_call"};

  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _))
      .Times(0);

  ITool *itool = &tool;
  auto result = itool->execute(doc, ctx);

  EXPECT_FALSE(result.success);
  ASSERT_EQ(mockAgent.mockPerms_->requestedCommands_.size(), 1u);
  EXPECT_EQ(mockAgent.mockPerms_->requestedCommands_[0], "sleep 10");
}

TEST_F(CommandPermissionToolTest,
       pythonExecute_deniedCommandDoesNotWriteTempFile) {
  PythonExecuteTool tool;
  mockAgent.mockPerms_->commandApprovalResponse_ = PermissionResponse::Deny;

  auto json = createJsonInput({{"code", "print('hi')"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _))
      .Times(0);

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  ASSERT_EQ(mockAgent.mockPerms_->requestedCommands_.size(), 1u);
  EXPECT_THAT(mockAgent.mockPerms_->requestedCommands_[0],
              HasSubstr("python3 /tmp/firmius_script_"));
}

TEST_F(CommandPermissionToolTest,
       fileEdit_deniedWriteApprovalDoesNotWriteFile) {
  FileEditTool tool;
  mockAgent.mockPerms_->editApprovalResponse_ = PermissionResponse::Deny;

  auto json =
      createJsonInput({{"path", "blocked.txt"}, {"content", "new content"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  EXPECT_CALL(mockHost, exists("/tmp/work/blocked.txt")).WillOnce(Return(false));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  ASSERT_EQ(mockAgent.mockPerms_->requestedEditPaths_.size(), 1u);
  EXPECT_EQ(mockAgent.mockPerms_->requestedEditPaths_[0],
            "/tmp/work/blocked.txt");
}

TEST_F(GrepToolTest, contextLines) {
  ProcessResult result;
  result.exitCode = 0;
  result.stdoutData = "/tmp/file.txt-5-Before context\n/tmp/file.txt:6:Match "
                      "line\n/tmp/file.txt-7-After context\n";

  EXPECT_CALL(mockHost, exec(HasSubstr("-B 2"), _, _, _))
      .WillOnce(Return(result));

  auto json = createJsonInput({{"pattern", "test"}, {"path", "/tmp"}},
                              {{"context_before", 2}, {"context_after", 3}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_TRUE(toolResult.success);
}

class ProcessExecuteToolTest : public ::testing::Test {
protected:
  ProcessExecuteTool tool;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;

  void SetUp() override {
    mockAgent.defaultCtx.environment.cwd = "/tmp/work";
    mockAgent.defaultCtx.permissions.allowedPaths = {"/tmp/work"};
    ON_CALL(mockAgent, getContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent, getMutableContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent, isInterrupted()).WillByDefault(Return(false));
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), resolvePath(_))
        .WillByDefault(Invoke([](const std::string &path) { return path; }));
  }
};

TEST_F(ProcessExecuteToolTest, cwdDefaultsToAgentCwd) {
  std::string capturedCwd;

  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _))
      .WillOnce(Invoke([&capturedCwd](const std::string &, const std::string &,
                                      const std::string &cwd, const auto &) {
        capturedCwd = cwd;
        return "proc_123";
      }));

  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), inspectProcess(_))
      .WillRepeatedly(Return(ProcessSnapshot{false, 0, "output", "", 100.0}));

  auto json = createJsonInput({{"command", "echo test"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  itool->execute(json, ctx);

  EXPECT_EQ(capturedCwd, "/tmp/work");
}

TEST_F(ProcessExecuteToolTest, timeoutHandling) {
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _)).WillOnce(Return("proc_123"));

  ProcessSnapshot runningSnapshot{true, -1, "partial output", "", 100.0};
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), inspectProcess(_))
      .WillRepeatedly(Return(runningSnapshot));

  auto json = createJsonInput({{"command", "sleep 100"}}, {{"timeout_ms", 50}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("Timeout"), std::string::npos);
}

class ListDirectoryToolTest : public ::testing::Test {
protected:
  ListDirectoryTool tool;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;

  void SetUp() override {
    mockAgent.defaultCtx.environment.cwd = "/tmp/work";
    mockAgent.defaultCtx.permissions.allowOutsideCwd = false;
    mockAgent.defaultCtx.permissions.allowedPaths = {"/work"};

    mockAgent.mockPerms_->allowOutsideCwd_ = false;
    mockAgent.mockPerms_->allowedPaths_ = {"/work"};
    mockAgent.mockPerms_->cwd_ = "/tmp/work";

    ON_CALL(mockAgent, getContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent, getMutableContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), resolvePath(_))
        .WillByDefault(Invoke([](const std::string &path) {
          if (path.starts_with("/"))
            return path;
          return "/tmp/work/" + path;
        }));
  }
};

TEST_F(ListDirectoryToolTest, allowedPaths_enforced) {
  mockAgent.defaultCtx.permissions.allowedPaths = {};
  mockAgent.defaultCtx.permissions.allowOutsideCwd = false;

  mockAgent.mockPerms_->allowOutsideCwd_ = false;
  mockAgent.mockPerms_->allowedPaths_ = {};

  auto json = createJsonInput({{"path", "/etc"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Access denied"), std::string::npos);
}

TEST_F(ListDirectoryToolTest, includeHidden_respected) {
  std::vector<FileInfo> entries;
  entries.push_back({".hidden", "/work/.hidden", 0, false, false, 0});
  entries.push_back({"visible", "/work/visible", 0, false, false, 0});

  EXPECT_CALL(mockHost, listDir("/work")).WillOnce(Return(entries));

  auto json = createJsonInput({{"path", "/work"}, {"include_hidden", "false"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.data.find(".hidden"), std::string::npos);
  EXPECT_NE(result.data.find("visible"), std::string::npos);
}
