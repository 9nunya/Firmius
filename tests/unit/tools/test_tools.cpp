#include "Events.hpp"
#include "IAgent.hpp"
#include "ITool.hpp"
#include "hosts/LocalHost.hpp"
#include "tools/FileReadTool.hpp"
#include "tools/GlobTool.hpp"
#include "tools/GrepTool.hpp"
#include "tools/ListDirectoryTool.hpp"
#include "tools/ProcessExecuteTool.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

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

class MockAgent : public IAgent {
public:
  MOCK_METHOD(void, reset, (), (override));
  MOCK_METHOD(void, run,
              (const std::string &, (std::function<void(const StreamEvent &)>)),
              (override));
  MOCK_METHOD((const AgentContext &), getContext, (), (const, override));
  MOCK_METHOD(AgentContext &, getMutableContext, (), (override));
  MOCK_METHOD(std::string, resolvePath, (const std::string &),
              (const, override));
  MOCK_METHOD(void, interrupt, (), (override));
  MOCK_METHOD(bool, isInterrupted, (), (const, override));
  MOCK_METHOD(void, setModel, (const std::string &, const std::string &),
              (override));
  MOCK_METHOD(bool, isRunning, (), (const, override));
  MOCK_METHOD(void, emitProcessSpawned,
              (const std::string &, const std::string &, const std::string &),
              (override));
  MOCK_METHOD(std::string, spawnProcess,
              (const std::string &, const std::string &, const std::string &,
               (const std::map<std::string, std::string> &)),
              (override));
  MOCK_METHOD(ProcessSnapshot, inspectProcess, (const std::string &),
              (override));
  MOCK_METHOD(void, writeToProcess, (const std::string &, const std::string &),
              (override));
  MOCK_METHOD(void, registerProcessId, (const std::string &), (override));
  MOCK_METHOD(void, addBlockingProcessId, (const std::string &), (override));
  MOCK_METHOD(void, removeBlockingProcessId, (const std::string &), (override));
  MOCK_METHOD((std::vector<std::string>), getBlockingProcessIds, (),
              (override));
  MOCK_METHOD(bool, hasReadFile, (const std::string &), (const, override));
  MOCK_METHOD(void, markFileAsRead, (const std::string &), (override));
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
    agentContext.environment.cwd = "/work";
    agentContext.permissions.allowOutsideCwd = false;
    agentContext.permissions.allowedPaths = {"/work", "/tmp"};

    ON_CALL(mockAgent, getContext()).WillByDefault(ReturnRef(agentContext));
    ON_CALL(mockAgent, getMutableContext())
        .WillByDefault(ReturnRef(agentContext));
    ON_CALL(mockAgent, resolvePath(_))
        .WillByDefault(Invoke([](const std::string &path) {
          if (path.starts_with("/"))
            return path;
          return "/work/" + path;
        }));
  }
};

TEST_F(FileReadToolTest, allowedPaths_permitsInside) {
  std::string content = "Line 1\nLine 2\nLine 3\n";
  std::vector<uint8_t> data(content.begin(), content.end());

  EXPECT_CALL(mockHost, readFile("/work/file.txt")).WillOnce(Return(data));

  auto json = createJsonInput({{"path", "file.txt"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("Line 1"), std::string::npos);
}

TEST_F(FileReadToolTest, allowedPaths_blocksOutside) {
  agentContext.permissions.allowedPaths = {};
  agentContext.permissions.allowOutsideCwd = false;

  auto json = createJsonInput({{"path", "/etc/passwd"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Access denied"), std::string::npos);
}

TEST_F(FileReadToolTest, lineSlicing) {
  std::string content = "Line 1\nLine 2\nLine 3\nLine 4\nLine 5\n";
  std::vector<uint8_t> data(content.begin(), content.end());

  EXPECT_CALL(mockHost, readFile("/work/file.txt")).WillOnce(Return(data));

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
    ON_CALL(mockAgent, resolvePath(_))
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
    ON_CALL(mockAgent, resolvePath(_))
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
  AgentContext agentContext;

  void SetUp() override {
    agentContext.environment.cwd = "/work";
    ON_CALL(mockAgent, getContext()).WillByDefault(ReturnRef(agentContext));
    ON_CALL(mockAgent, getMutableContext())
        .WillByDefault(ReturnRef(agentContext));
    ON_CALL(mockAgent, isInterrupted()).WillByDefault(Return(false));
    ON_CALL(mockAgent, resolvePath(_)).WillByDefault(Invoke([](const std::string& path) { return path; }));
  }
};

TEST_F(ProcessExecuteToolTest, cwdDefaultsToAgentCwd) {
  std::string capturedCwd;

  EXPECT_CALL(mockAgent, spawnProcess(_, _, _, _))
      .WillOnce(Invoke([&capturedCwd](const std::string &, const std::string &,
                                      const std::string &cwd, const auto &) {
        capturedCwd = cwd;
        return "proc_123";
      }));

  EXPECT_CALL(mockAgent, inspectProcess(_))
      .WillRepeatedly(Return(ProcessSnapshot{false, 0, "output", "", 100.0}));

  auto json = createJsonInput({{"command", "echo test"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  itool->execute(json, ctx);

  EXPECT_EQ(capturedCwd, "/work");
}

TEST_F(ProcessExecuteToolTest, timeoutHandling) {
  EXPECT_CALL(mockAgent, spawnProcess(_, _, _, _)).WillOnce(Return("proc_123"));

  ProcessSnapshot runningSnapshot{true, -1, "partial output", "", 100.0};
  EXPECT_CALL(mockAgent, inspectProcess(_))
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
  AgentContext agentContext;

  void SetUp() override {
    agentContext.environment.cwd = "/work";
    agentContext.permissions.allowOutsideCwd = false;
    agentContext.permissions.allowedPaths = {"/work"};

    ON_CALL(mockAgent, getContext()).WillByDefault(ReturnRef(agentContext));
    ON_CALL(mockAgent, getMutableContext())
        .WillByDefault(ReturnRef(agentContext));
    ON_CALL(mockAgent, resolvePath(_))
        .WillByDefault(Invoke([](const std::string &path) {
          if (path.starts_with("/"))
            return path;
          return "/work/" + path;
        }));
  }
};

TEST_F(ListDirectoryToolTest, allowedPaths_enforced) {
  agentContext.permissions.allowedPaths = {};
  agentContext.permissions.allowOutsideCwd = false;

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
