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
#include "tools/ProcessStatusTool.hpp"
#include "tools/ProcessSpawnTool.hpp"
#include "tools/PythonExecuteTool.hpp"
#include "tools/ToolRegistry.hpp"
#include "utils/Hashline.hpp"
#include "AgentRegistry.hpp"
#include <filesystem>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <set>
#include <atomic>
#include <thread>

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
  MOCK_METHOD(void, resume, ((std::function<void(const StreamEvent &)>)),
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

rapidjson::Document createFileEditJson(
    const std::string &path,
    const std::vector<std::map<std::string, std::string>> &edits) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("path", makeJsonString(path, alloc), alloc);

  rapidjson::Value editArray(rapidjson::kArrayType);
  for (const auto &editFields : edits) {
    rapidjson::Value editObj(rapidjson::kObjectType);
    rapidjson::Value newLines(rapidjson::kArrayType);

    for (const auto &[key, value] : editFields) {
      if (key == "new_lines") {
        std::stringstream ss(value);
        std::string line;
        while (std::getline(ss, line, '\n')) {
          newLines.PushBack(makeJsonString(line, alloc), alloc);
        }
        continue;
      }
      editObj.AddMember(makeJsonString(key, alloc), makeJsonString(value, alloc),
                        alloc);
    }

    if (!newLines.Empty()) {
      editObj.AddMember("new_lines", newLines, alloc);
    }
    editArray.PushBack(editObj, alloc);
  }

  doc.AddMember("edits", editArray, alloc);
  return doc;
}

void addFileEditEdits(rapidjson::Document &doc,
                      const std::vector<std::map<std::string, std::string>> &edits) {
  auto &alloc = doc.GetAllocator();
  rapidjson::Value editArray(rapidjson::kArrayType);
  for (const auto &editFields : edits) {
    rapidjson::Value editObj(rapidjson::kObjectType);
    rapidjson::Value newLines(rapidjson::kArrayType);

    for (const auto &[key, value] : editFields) {
      if (key == "new_lines") {
        std::stringstream ss(value);
        std::string line;
        while (std::getline(ss, line, '\n')) {
          newLines.PushBack(makeJsonString(line, alloc), alloc);
        }
        continue;
      }
      editObj.AddMember(makeJsonString(key, alloc), makeJsonString(value, alloc),
                        alloc);
    }

    if (!newLines.Empty()) {
      editObj.AddMember("new_lines", newLines, alloc);
    }
    editArray.PushBack(editObj, alloc);
  }

  doc.AddMember("edits", editArray, alloc);
}

void addFileEditLegacyNoise(rapidjson::Document &doc,
                            const std::string &oldString = "",
                            const std::string &newString = "",
                            bool replaceAll = false,
                            float fuzzyThreshold = 0.0f) {
  auto &alloc = doc.GetAllocator();
  doc.AddMember("old_string", makeJsonString(oldString, alloc), alloc);
  doc.AddMember("new_string", makeJsonString(newString, alloc), alloc);
  doc.AddMember("replace_all", replaceAll, alloc);
  doc.AddMember("fuzzy_threshold", fuzzyThreshold, alloc);
}

TEST(ToolContextCancellationContractTest, CancelRequestedReflectsSignal) {
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  std::atomic<bool> cancelled{false};
  ToolContext ctx{mockHost, mockAgent, "tool-call-1", &cancelled};
  EXPECT_FALSE(ctx.cancelRequested());
  cancelled.store(true);
  EXPECT_TRUE(ctx.cancelRequested());
}

TEST(ToolContextCancellationContractTest, WaitForReturnsFalseWhenCancelled) {
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  std::atomic<bool> cancelled{false};
  ToolContext ctx{mockHost, mockAgent, "tool-call-2", &cancelled};
  std::thread interrupter([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cancelled.store(true);
  });
  EXPECT_FALSE(ctx.waitFor(std::chrono::milliseconds(200),
                           std::chrono::milliseconds(5)));
  interrupter.join();
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
  std::map<std::string, FileInfo> fileInfos;
  std::map<std::string, std::vector<FileInfo>> dirEntries;

  void addEntry(const std::string &path, bool isDirectory) {
    FileInfo info;
    info.path = path;
    info.name = std::filesystem::path(path).filename().string();
    info.isDirectory = isDirectory;
    fileInfos[path] = info;

    const std::string parent = std::filesystem::path(path).parent_path().string();
    if (!parent.empty() && parent != path) {
      dirEntries[parent].push_back(info);
    }
    if (isDirectory && !dirEntries.count(path)) {
      dirEntries[path] = {};
    }
  }

  void SetUp() override {
    mockAgent.defaultCtx.permissions.allowedPaths = {"/tmp", "/root"};
    ON_CALL(mockAgent, getContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), resolvePath(_))
        .WillByDefault(Invoke([](const std::string &path) { return path; }));
    ON_CALL(mockHost, stat(_))
        .WillByDefault(Invoke([this](const std::string &path) {
          auto it = fileInfos.find(path);
          if (it == fileInfos.end()) {
            throw std::runtime_error("Path not found: " + path);
          }
          return it->second;
        }));
    ON_CALL(mockHost, listDir(_))
        .WillByDefault(Invoke([this](const std::string &path) {
          auto it = dirEntries.find(path);
          if (it == dirEntries.end()) {
            throw std::runtime_error("Path not found: " + path);
          }
          return it->second;
        }));
  }
};

TEST_F(GlobToolTest, noMatchesReturnsEmptyArray) {
  addEntry("/tmp", true);
  addEntry("/tmp/file.cpp", false);

  auto json = createJsonInput({{"pattern", "*.nonexistent"}, {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_TRUE(toolResult.success);
  EXPECT_NE(toolResult.data.find("[]"), std::string::npos);
}

TEST_F(GlobToolTest, listDirFailureReturnsError) {
  addEntry("/root", true);
  EXPECT_CALL(mockHost, listDir("/root"))
      .WillOnce(::testing::Throw(std::runtime_error("Permission denied")));

  auto json = createJsonInput({{"pattern", "*.txt"}, {"path", "/root"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_FALSE(toolResult.success);
  EXPECT_NE(toolResult.error.find("Permission denied"), std::string::npos);
}

TEST_F(GlobToolTest, patternMatchingSupportsRecursiveAndBraceWildcards) {
  addEntry("/tmp", true);
  addEntry("/tmp/src", true);
  addEntry("/tmp/src/nested", true);
  addEntry("/tmp/file1.txt", false);
  addEntry("/tmp/src/file2.txt", false);
  addEntry("/tmp/src/nested/file3.log", false);
  addEntry("/tmp/src/nested/file4.txt", false);

  auto json =
      createJsonInput({{"pattern", "**/*.{txt,log}"}, {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_TRUE(toolResult.success);
  EXPECT_NE(toolResult.data.find("file1.txt"), std::string::npos);
  EXPECT_NE(toolResult.data.find("file2.txt"), std::string::npos);
  EXPECT_NE(toolResult.data.find("file3.log"), std::string::npos);
  EXPECT_NE(toolResult.data.find("file4.txt"), std::string::npos);
}

TEST_F(GlobToolTest, patternMatchingSupportsCharacterClassesAndPathSegments) {
  addEntry("/tmp", true);
  addEntry("/tmp/src", true);
  addEntry("/tmp/src/foo", true);
  addEntry("/tmp/src/bar", true);
  addEntry("/tmp/src/foo/test1.cpp", false);
  addEntry("/tmp/src/foo/test2.cpp", false);
  addEntry("/tmp/src/bar/testA.cpp", false);

  auto json =
      createJsonInput({{"pattern", "src/**/test[0-9].cpp"}, {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_TRUE(toolResult.success);
  EXPECT_NE(toolResult.data.find("test1.cpp"), std::string::npos);
  EXPECT_NE(toolResult.data.find("test2.cpp"), std::string::npos);
  EXPECT_EQ(toolResult.data.find("testA.cpp"), std::string::npos);
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

  EXPECT_CALL(mockHost, exec(HasSubstr("rg --json --pcre2"), _, _, _))
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

  EXPECT_CALL(mockHost, exec(HasSubstr("rg --json --pcre2"), _, _, _))
      .WillOnce(Return(result));

  auto json = createJsonInput(
      {{"pattern", "nonexistent_pattern_12345"}, {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_TRUE(toolResult.success);
  EXPECT_NE(toolResult.data.find("[]"), std::string::npos);
}

TEST_F(GrepToolTest, parsesRipgrepJsonAndSupportsAdvancedRegexEngine) {
  ProcessResult result;
  result.exitCode = 0;
  result.stdoutData =
      R"({"type":"match","data":{"path":{"text":"/tmp/dir:with:colon/file.cpp"},"lines":{"text":"int value = 42;\n"},"line_number":6}})"
      "\n"
      R"({"type":"context","data":{"path":{"text":"/tmp/dir:with:colon/file.cpp"},"lines":{"text":"// before\n"},"line_number":5}})"
      "\n";

  EXPECT_CALL(mockHost, exec(HasSubstr("rg --json --pcre2"), _, _, _))
      .WillOnce(Return(result));

  auto json = createJsonInput({{"pattern", R"((?<=value\s=\s)\d+)"},
                               {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_TRUE(toolResult.success);
  EXPECT_NE(toolResult.data.find("/tmp/dir:with:colon/file.cpp"),
            std::string::npos);
  EXPECT_NE(toolResult.data.find("\"line\":6"), std::string::npos);
  EXPECT_NE(toolResult.data.find("\"line\":5"), std::string::npos);
}

TEST_F(GrepToolTest, fallsBackToPerlGrepWhenRipgrepIsUnavailable) {
  ProcessResult unavailable;
  unavailable.exitCode = 127;
  unavailable.stderrData = "rg: not found";

  ProcessResult fallback;
  fallback.exitCode = 0;
  fallback.stdoutData = "/tmp/file.txt:7:lookbehind match\n";

  ::testing::InSequence sequence;
  EXPECT_CALL(mockHost, exec(HasSubstr("rg --json --pcre2"), _, _, _))
      .WillOnce(Return(unavailable));
  EXPECT_CALL(mockHost, exec(HasSubstr("grep -rnHP"), _, _, _))
      .WillOnce(Return(fallback));

  auto json = createJsonInput({{"pattern", R"((?<=foo)bar)"}, {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto toolResult = itool->execute(json, ctx);

  EXPECT_TRUE(toolResult.success);
  EXPECT_NE(toolResult.data.find("\"line\":7"), std::string::npos);
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

class FileEditAnchorToolTest : public ::testing::Test {
protected:
  FileEditTool tool;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  std::string capturedWrite;

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
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), hasFullyReadFile(_))
        .WillByDefault(Return(true));
    ON_CALL(mockHost, writeFile(_, _))
        .WillByDefault(Invoke([this](const std::string &,
                                     const std::vector<uint8_t> &data) {
          capturedWrite.assign(data.begin(), data.end());
        }));
  }

  static std::vector<uint8_t> bytes(const std::string &text) {
    return std::vector<uint8_t>(text.begin(), text.end());
  }

  static std::string anchor(int line, const std::string &content) {
    return firmius::shared::utils::Hashline::formatAnchor(line, content);
  }

  static std::string readFormattedAnchor(int line, const std::string &content) {
    return firmius::shared::utils::Hashline::formatLine(line, content);
  }

  static rapidjson::Document parseResult(const ToolResult &result) {
    rapidjson::Document doc;
    doc.Parse(result.data.c_str());
    return doc;
  }
};

class ToolRegistryArgumentNormalizationTest : public ::testing::Test {
protected:
  ToolRegistry registry;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  std::string capturedWrite;

  void SetUp() override {
    registry.registerTool(std::make_unique<FileEditTool>());

    mockAgent.defaultCtx.environment.cwd = "/tmp/work";
    mockAgent.defaultCtx.permissions.allowOutsideCwd = false;
    mockAgent.defaultCtx.permissions.allowedPaths = {"/tmp/work", "/tmp"};
    mockAgent.defaultCtx.permissions.allowedScopes = {
        ToolScope::FilesystemWrite};
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
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), hasFullyReadFile(_))
        .WillByDefault(Return(true));
    ON_CALL(mockHost, writeFile(_, _))
        .WillByDefault(Invoke([this](const std::string &,
                                     const std::vector<uint8_t> &data) {
          capturedWrite.assign(data.begin(), data.end());
        }));
  }

  static std::vector<uint8_t> bytes(const std::string &text) {
    return std::vector<uint8_t>(text.begin(), text.end());
  }

  static std::string anchor(int line, const std::string &content) {
    return firmius::shared::utils::Hashline::formatAnchor(line, content);
  }

  ToolResult executeFileEdit(rapidjson::Document &input) {
    ToolContext ctx{mockHost, mockAgent, "test_call"};
    return registry.execute("file_edit", input, ctx);
  }
};

TEST_F(CommandPermissionToolTest,
       processExecute_deniedCommandDoesNotSpawnProcess) {
  ProcessExecuteTool tool;
  mockAgent.mockPerms_->commandApprovalResponse_ = PermissionResponse::Deny;

  auto json =
      createJsonInput({{"command", "touch denied.txt"}, {"cwd", "/tmp/work"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _, _))
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

  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _, _))
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
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _, _))
      .Times(0);

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  ASSERT_EQ(mockAgent.mockPerms_->requestedCommands_.size(), 1u);
  EXPECT_EQ(mockAgent.mockPerms_->requestedCommands_[0],
            "python3 -c \"import os; exec(compile(os.environ['FIRMIUS_PYTHON_EXECUTE_CODE'], '<python_execute>', 'exec'))\"");
}

TEST_F(CommandPermissionToolTest,
       pythonExecuteUsesSingleApprovedCommandWithEnvPayload) {
  PythonExecuteTool tool;
  std::map<std::string, std::string> capturedEnv;

  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _, _))
      .WillOnce(Invoke([&capturedEnv](const std::string &command,
                                      const std::string &, const std::string &,
                                      const auto &env, bool) {
        EXPECT_EQ(command,
                  "python3 -c \"import os; exec(compile(os.environ['FIRMIUS_PYTHON_EXECUTE_CODE'], '<python_execute>', 'exec'))\"");
        capturedEnv = env;
        return "proc_python";
      }));
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), inspectProcess(_))
      .WillRepeatedly(Return(ProcessSnapshot{false, 0, "ok", "", 5.0}));

  auto json = createJsonInput({{"code", "print('hi')"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(mockAgent.mockPerms_->requestedCommands_.size(), 1u);
  EXPECT_EQ(mockAgent.mockPerms_->requestedCommands_[0],
            "python3 -c \"import os; exec(compile(os.environ['FIRMIUS_PYTHON_EXECUTE_CODE'], '<python_execute>', 'exec'))\"");
  ASSERT_EQ(capturedEnv["FIRMIUS_PYTHON_EXECUTE_CODE"], "print('hi')");
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

TEST_F(FileEditAnchorToolTest, replaceRangeByAnchor) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\ngamma\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "replace_range"},
        {"start_anchor", anchor(2, "beta")},
        {"end_anchor", anchor(3, "gamma")},
        {"new_lines", "beta2\ngamma2"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "alpha\nbeta2\ngamma2\n");
  EXPECT_NE(result.data.find("\"applied_edits\":1"), std::string::npos);
  EXPECT_NE(result.data.find("\"post_edit_slice\""), std::string::npos);
  EXPECT_NE(result.data.find("2#"), std::string::npos);

  auto doc = parseResult(result);
  ASSERT_TRUE(doc.HasMember("operations"));
  const auto &operation = doc["operations"].GetArray()[0];
  ASSERT_TRUE(operation.HasMember("post_edit_context"));
  const auto &context = operation["post_edit_context"];
  EXPECT_EQ(context["start_line"].GetInt(), 1);
  EXPECT_EQ(context["end_line"].GetInt(), 3);
  ASSERT_TRUE(context.HasMember("anchors"));
  EXPECT_EQ(context["anchors"].GetArray().Size(), 3u);
  EXPECT_STREQ(context["anchors"].GetArray()[1].GetString(),
               anchor(2, "beta2").c_str());
}

TEST_F(FileEditAnchorToolTest,
       replaceRangeSanitizesHashlinePrefixesDiffMarkersAndBoundaryEchoes) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "keep-a\nbeta\ngamma\nkeep-b\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "replace_range"},
        {"start_anchor", anchor(2, "beta")},
        {"end_anchor", anchor(3, "gamma")},
        {"new_lines",
         "keep-a\n2#8c72|+ beta2\n3#f2c5|48a8|- gamma2\nkeep-b"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "keep-a\nbeta2\ngamma2\nkeep-b\n");

  auto doc = parseResult(result);
  ASSERT_TRUE(doc.IsObject());
  ASSERT_TRUE(doc.HasMember("sanitation"));
  const auto &sanitation = doc["sanitation"];
  EXPECT_EQ(sanitation["hashline_prefixes_stripped"].GetInt(), 2);
  EXPECT_EQ(sanitation["malformed_hash_fragments_stripped"].GetInt(), 1);
  EXPECT_EQ(sanitation["diff_markers_stripped"].GetInt(), 2);
  EXPECT_EQ(sanitation["boundary_echoes_removed"].GetInt(), 2);
  EXPECT_TRUE(sanitation["boundary_echo_removed"].GetBool());

  ASSERT_TRUE(doc.HasMember("operations"));
  const auto &operation = doc["operations"].GetArray()[0];
  ASSERT_TRUE(operation.HasMember("old_lines"));
  ASSERT_TRUE(operation.HasMember("new_lines"));
  ASSERT_EQ(operation["old_lines"].GetArray().Size(), 2u);
  EXPECT_STREQ(operation["old_lines"].GetArray()[0].GetString(), "beta");
  EXPECT_STREQ(operation["old_lines"].GetArray()[1].GetString(), "gamma");
  ASSERT_EQ(operation["new_lines"].GetArray().Size(), 2u);
  EXPECT_STREQ(operation["new_lines"].GetArray()[0].GetString(), "beta2");
  EXPECT_STREQ(operation["new_lines"].GetArray()[1].GetString(), "gamma2");
}

TEST_F(FileEditAnchorToolTest, insertAfterByAnchor) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "insert_after"},
        {"anchor", anchor(1, "alpha")},
        {"new_lines", "inserted"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "alpha\ninserted\nbeta\n");
}

TEST_F(FileEditAnchorToolTest, insertAfterRejectsReadFormattedAnchorWithHelp) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "insert_after"},
        {"anchor", readFormattedAnchor(1, "alpha")},
        {"new_lines", "inserted"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Malformed anchor"), std::string::npos);
  EXPECT_NE(result.error.find("lineNumber#hash only"), std::string::npos);
  EXPECT_NE(result.error.find("without trailing |content"), std::string::npos);
}

TEST_F(FileEditAnchorToolTest, replaceRangeWithAnchorOnlyFailsPrecisely) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "replace_range"}, {"anchor", anchor(2, "beta")}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find(
                "replace_range requires both start_anchor and end_anchor; got "
                "anchor only"),
            std::string::npos);
}

TEST_F(FileEditAnchorToolTest, replaceRangeMissingEndAnchorFailsPrecisely) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "replace_range"}, {"start_anchor", anchor(2, "beta")}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("replace_range is missing end_anchor"),
            std::string::npos);
}

TEST_F(FileEditAnchorToolTest, deleteRangeMissingStartAnchorFailsPrecisely) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "delete_range"}, {"end_anchor", anchor(2, "beta")}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("delete_range is missing start_anchor"),
            std::string::npos);
}

TEST_F(FileEditAnchorToolTest, insertBeforeByAnchor) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "insert_before"},
        {"anchor", anchor(2, "beta")},
        {"new_lines", "inserted"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "alpha\ninserted\nbeta\n");
}

TEST_F(FileEditAnchorToolTest, deleteRangeByAnchor) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\ngamma\ndelta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "delete_range"},
        {"start_anchor", anchor(2, "beta")},
        {"end_anchor", anchor(3, "gamma")}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "alpha\ndelta\n");

  auto doc = parseResult(result);
  const auto &operation = doc["operations"].GetArray()[0];
  ASSERT_EQ(operation["old_lines"].GetArray().Size(), 2u);
  EXPECT_STREQ(operation["old_lines"].GetArray()[0].GetString(), "beta");
  EXPECT_STREQ(operation["old_lines"].GetArray()[1].GetString(), "gamma");
  EXPECT_TRUE(operation["new_lines"].IsArray());
  EXPECT_TRUE(operation["new_lines"].GetArray().Empty());
}

TEST_F(FileEditAnchorToolTest, insertAfterResultIncludesEmptyOldLinesAndNewLines) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "insert_after"},
        {"anchor", anchor(1, "alpha")},
        {"new_lines", "inserted"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  auto doc = parseResult(result);
  const auto &operation = doc["operations"].GetArray()[0];
  EXPECT_TRUE(operation["old_lines"].IsArray());
  EXPECT_TRUE(operation["old_lines"].GetArray().Empty());
  ASSERT_EQ(operation["new_lines"].GetArray().Size(), 1u);
  EXPECT_STREQ(operation["new_lines"].GetArray()[0].GetString(), "inserted");
}

TEST_F(FileEditAnchorToolTest, suspiciousReplacementMetadataIsRejected) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "replace_range"},
        {"start_anchor", anchor(2, "beta")},
        {"end_anchor", anchor(2, "beta")},
        {"new_lines", "48a8|still bad"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("still appears to contain Hashline metadata"),
            std::string::npos);
  EXPECT_NE(result.error.find("Remove lineNumber#hash| prefixes"),
            std::string::npos);

  rapidjson::Document doc;
  doc.Parse(result.error.c_str());
  ASSERT_TRUE(doc.HasMember("sanitation"));
  EXPECT_TRUE(doc["sanitation"]["suspicious_content_found"].GetBool());
  EXPECT_TRUE(doc["sanitation"]["suspicious_content_rejected"].GetBool());
}

TEST_F(FileEditAnchorToolTest, suspiciousReplacementDiffJunkIsRejected) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "replace_range"},
        {"start_anchor", anchor(2, "beta")},
        {"end_anchor", anchor(2, "beta")},
        {"new_lines", "+still bad"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("still appears to contain diff markers"),
            std::string::npos);
  EXPECT_NE(result.error.find("Remove leading + / - patch markers"),
            std::string::npos);
}

TEST_F(FileEditAnchorToolTest, staleAnchorFailsClearly) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "insert_after"},
        {"anchor", "2#dead"},
        {"new_lines", "inserted"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Stale anchor"), std::string::npos);
  EXPECT_NE(result.error.find("Re-read the file and retry with fresh anchors"),
            std::string::npos);
}

TEST_F(FileEditAnchorToolTest, nearbyAnchorRelocationSucceeds) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "prefix\nalpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "insert_after"},
        {"anchor", anchor(2, "beta")},
        {"new_lines", "inserted"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "prefix\nalpha\nbeta\ninserted\n");
  EXPECT_NE(result.data.find("\"relocated_anchors\":1"), std::string::npos);
}

TEST_F(FileEditAnchorToolTest, overlappingEditsAreRejected) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\ngamma\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "replace_range"},
        {"start_anchor", anchor(2, "beta")},
        {"end_anchor", anchor(3, "gamma")},
        {"new_lines", "beta2"}},
       {{"op", "insert_before"},
        {"anchor", anchor(3, "gamma")},
        {"new_lines", "inserted"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Overlapping edits"), std::string::npos);
}

TEST_F(FileEditAnchorToolTest, overwriteExistingFileIsRejected) {
  const std::string path = "/tmp/work/file.txt";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json =
      createJsonInput({{"path", "file.txt"}, {"content", "replacement"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("overwrite is disabled for existing files"),
            std::string::npos);
}

TEST_F(FileEditAnchorToolTest,
       contentAcceptsEmptyEditsAndDefaultLegacyTransportNoise) {
  const std::string path = "/tmp/work/file.txt";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(false));
  EXPECT_CALL(mockHost, writeFile(path, _)).WillOnce(Invoke(
      [this](const std::string &, const std::vector<uint8_t> &data) {
        capturedWrite.assign(data.begin(), data.end());
      }));

  auto json = createJsonInput({{"path", "file.txt"}, {"content", "replacement"}});
  addFileEditEdits(json, {});
  addFileEditLegacyNoise(json, "", "", false, 0.0f);

  ToolContext ctx{mockHost, mockAgent, "test_call"};
  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "replacement");
  EXPECT_NE(result.data.find("\"mode\":\"overwrite\""), std::string::npos);
}

TEST_F(FileEditAnchorToolTest,
       contentAcceptsDefaultLegacyFlagsWithoutRealStrings) {
  const std::string path = "/tmp/work/file.txt";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(false));
  EXPECT_CALL(mockHost, writeFile(path, _)).WillOnce(Invoke(
      [this](const std::string &, const std::vector<uint8_t> &data) {
        capturedWrite.assign(data.begin(), data.end());
      }));

  auto json = createJsonInput({{"path", "file.txt"}, {"content", "replacement"}});
  addFileEditLegacyNoise(json, "", "", false, 0.0f);

  ToolContext ctx{mockHost, mockAgent, "test_call"};
  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "replacement");
  EXPECT_NE(result.data.find("\"mode\":\"overwrite\""), std::string::npos);
}

TEST_F(FileEditAnchorToolTest, contentRejectsNonEmptyHashlineEdits) {
  const std::string path = "/tmp/work/file.txt";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(false));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json = createJsonInput({{"path", "file.txt"}, {"content", "replacement"}});
  addFileEditEdits(json, {{{"op", "insert_after"},
                           {"anchor", "1#abcd"},
                           {"new_lines", "inserted"}}});
  addFileEditLegacyNoise(json, "", "", false, 0.0f);

  ToolContext ctx{mockHost, mockAgent, "test_call"};
  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("exactly one editing mode"), std::string::npos);
}

TEST_F(FileEditAnchorToolTest, contentRejectsRealLegacyReplacement) {
  const std::string path = "/tmp/work/file.txt";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(false));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json = createJsonInput({{"path", "file.txt"}, {"content", "replacement"}});
  addFileEditLegacyNoise(json, "old text", "new text", false, 0.0f);

  ToolContext ctx{mockHost, mockAgent, "test_call"};
  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("exactly one editing mode"), std::string::npos);
}

TEST_F(FileEditAnchorToolTest, emptyLegacyDefaultsDoNotActivateLegacyMode) {
  const std::string path = "/tmp/work/file.txt";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(false));

  auto json = createJsonInput({{"path", "file.txt"}});
  addFileEditLegacyNoise(json, "", "", false, 0.0f);

  ToolContext ctx{mockHost, mockAgent, "test_call"};
  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Missing edits, content, or legacy replacement parameters."),
            std::string::npos);
}

TEST_F(FileEditAnchorToolTest, hashlineEditsStillWorkWithoutNoise) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  auto json = createFileEditJson(
      "file.txt",
      {{{"op", "insert_after"},
        {"anchor", anchor(1, "alpha")},
        {"new_lines", "inserted"}}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "alpha\ninserted\nbeta\n");
}

TEST_F(FileEditAnchorToolTest, mixingEditModesIsRejected) {
  const std::string path = "/tmp/work/file.txt";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(false));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  auto json =
      createJsonInput({{"path", "file.txt"}, {"content", "replacement"}});
  auto &alloc = json.GetAllocator();
  rapidjson::Value editArray(rapidjson::kArrayType);
  rapidjson::Value editObj(rapidjson::kObjectType);
  editObj.AddMember("op", makeJsonString("insert_after", alloc), alloc);
  editObj.AddMember("anchor", makeJsonString("1#abcd", alloc), alloc);
  rapidjson::Value newLines(rapidjson::kArrayType);
  newLines.PushBack(makeJsonString("inserted", alloc), alloc);
  editObj.AddMember("new_lines", newLines, alloc);
  editArray.PushBack(editObj, alloc);
  json.AddMember("edits", editArray, alloc);

  ToolContext ctx{mockHost, mockAgent, "test_call"};
  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("exactly one editing mode"), std::string::npos);
}

TEST_F(ToolRegistryArgumentNormalizationTest,
       nestedQuotedFileEditKeysAreNormalizedBeforeExecution) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("path", makeJsonString("file.txt", alloc), alloc);
  rapidjson::Value edits(rapidjson::kArrayType);
  rapidjson::Value edit(rapidjson::kObjectType);
  edit.AddMember("\"op\"", makeJsonString("insert_after", alloc), alloc);
  edit.AddMember("\"anchor\"", makeJsonString(anchor(1, "alpha"), alloc), alloc);
  rapidjson::Value newLines(rapidjson::kArrayType);
  newLines.PushBack(makeJsonString("inserted", alloc), alloc);
  edit.AddMember("\"new_lines\"", newLines, alloc);
  edits.PushBack(edit, alloc);
  input.AddMember("\"edits\"", edits, alloc);

  auto result = executeFileEdit(input);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "alpha\ninserted\nbeta\n");
}

TEST_F(ToolRegistryArgumentNormalizationTest,
       quotedNestedKeysSurfaceRealValidationErrors) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("path", makeJsonString("file.txt", alloc), alloc);
  rapidjson::Value edits(rapidjson::kArrayType);
  rapidjson::Value edit(rapidjson::kObjectType);
  edit.AddMember("\"op\"", makeJsonString("insert_after", alloc), alloc);
  edit.AddMember("\"anchor\"", makeJsonString("1#abcd|alpha", alloc), alloc);
  rapidjson::Value newLines(rapidjson::kArrayType);
  newLines.PushBack(makeJsonString("inserted", alloc), alloc);
  edit.AddMember("\"new_lines\"", newLines, alloc);
  edits.PushBack(edit, alloc);
  input.AddMember("\"edits\"", edits, alloc);

  auto result = executeFileEdit(input);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("Malformed anchor"));
  EXPECT_THAT(result.error,
              ::testing::Not(HasSubstr("Missing edits, content, or legacy replacement parameters.")));
}

TEST_F(ToolRegistryArgumentNormalizationTest,
       recursiveNormalizationWorksForArraysOfObjects) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\ngamma\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("path", makeJsonString("file.txt", alloc), alloc);
  rapidjson::Value edits(rapidjson::kArrayType);

  rapidjson::Value firstEdit(rapidjson::kObjectType);
  firstEdit.AddMember("\"op\"", makeJsonString("insert_after", alloc), alloc);
  firstEdit.AddMember("\"anchor\"", makeJsonString(anchor(1, "alpha"), alloc),
                      alloc);
  rapidjson::Value firstNewLines(rapidjson::kArrayType);
  firstNewLines.PushBack(makeJsonString("x", alloc), alloc);
  firstEdit.AddMember("\"new_lines\"", firstNewLines, alloc);
  edits.PushBack(firstEdit, alloc);

  rapidjson::Value secondEdit(rapidjson::kObjectType);
  secondEdit.AddMember("\"op\"", makeJsonString("insert_before", alloc), alloc);
  secondEdit.AddMember("\"anchor\"", makeJsonString(anchor(3, "gamma"), alloc),
                       alloc);
  rapidjson::Value secondNewLines(rapidjson::kArrayType);
  secondNewLines.PushBack(makeJsonString("y", alloc), alloc);
  secondEdit.AddMember("\"new_lines\"", secondNewLines, alloc);
  edits.PushBack(secondEdit, alloc);

  input.AddMember("\"edits\"", edits, alloc);

  auto result = executeFileEdit(input);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "alpha\nx\nbeta\ny\ngamma\n");
}

TEST_F(ToolRegistryArgumentNormalizationTest,
       alreadyCorrectArgumentsRemainUnaffected) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  auto input = createFileEditJson(
      "file.txt",
      {{{"op", "insert_after"},
        {"anchor", anchor(1, "alpha")},
        {"new_lines", "inserted"}}});

  auto result = executeFileEdit(input);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(capturedWrite, "alpha\ninserted\nbeta\n");
}

TEST_F(ToolRegistryArgumentNormalizationTest,
       normalizationDoesNotInventUnrelatedKeys) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("path", makeJsonString("file.txt", alloc), alloc);
  rapidjson::Value edits(rapidjson::kArrayType);
  rapidjson::Value edit(rapidjson::kObjectType);
  edit.AddMember("\"operation\"", makeJsonString("insert_after", alloc), alloc);
  edit.AddMember("\"anchor\"", makeJsonString(anchor(1, "alpha"), alloc), alloc);
  rapidjson::Value newLines(rapidjson::kArrayType);
  newLines.PushBack(makeJsonString("inserted", alloc), alloc);
  edit.AddMember("\"new_lines\"", newLines, alloc);
  edits.PushBack(edit, alloc);
  input.AddMember("\"edits\"", edits, alloc);

  auto result = executeFileEdit(input);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("Malformed edit op"));
}

TEST_F(GrepToolTest, contextLines) {
  ProcessResult result;
  result.exitCode = 0;
  result.stdoutData =
      R"({"type":"context","data":{"path":{"text":"/tmp/file.txt"},"lines":{"text":"Before context\n"},"line_number":5}})"
      "\n"
      R"({"type":"match","data":{"path":{"text":"/tmp/file.txt"},"lines":{"text":"Match line\n"},"line_number":6}})"
      "\n"
      R"({"type":"context","data":{"path":{"text":"/tmp/file.txt"},"lines":{"text":"After context\n"},"line_number":7}})"
      "\n";

  EXPECT_CALL(mockHost,
              exec(::testing::AllOf(HasSubstr("rg --json --pcre2"),
                                    HasSubstr("-B 2"), HasSubstr("-A 3")),
                   _, _, _))
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

  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _, _))
      .WillOnce(Invoke([&capturedCwd](const std::string &, const std::string &,
                                      const std::string &cwd, const auto &,
                                      bool) {
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
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _, _)).WillOnce(Return("proc_123"));

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

TEST_F(ProcessExecuteToolTest, blocksForeignApplyPatchCommand) {
  auto json = createJsonInput({{"command", "apply_patch <<'PATCH'\n*** Begin Patch\n*** End Patch\nPATCH"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("'apply_patch' is not available in Firmius"));
  EXPECT_THAT(result.error, HasSubstr("Use file_read + file_edit instead"));
}

TEST_F(ProcessExecuteToolTest, nonZeroExitReturnsFailureWithStructuredResult) {
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _, _))
      .WillOnce(Return("proc_123"));
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), inspectProcess(_))
      .WillRepeatedly(Return(ProcessSnapshot{false, 17, "out", "err", 10.0}));

  auto json = createJsonInput({{"command", "false"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("non-zero exit code: 17"));
  EXPECT_THAT(result.data, HasSubstr("\"exit_code\":17"));
  EXPECT_THAT(result.data, HasSubstr("\"command_success\":false"));
}

class ProcessStatusToolTest : public ::testing::Test {
protected:
  ProcessStatusTool tool;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;

  void SetUp() override {
    ON_CALL(mockAgent, getContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent, getMutableContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
  }
};

TEST_F(ProcessStatusToolTest, rejectsAgentIdWithActionableHint) {
  auto otherAgent = std::make_shared<NiceMock<MockAgent>>();
  AgentRegistry::instance().registerAgent("agent-123", otherAgent);

  auto json = createJsonInput({{"process_id", "agent-123"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};
  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  AgentRegistry::instance().unregisterAgent("agent-123");

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("belongs to a subagent"));
  EXPECT_THAT(result.error, HasSubstr("subagent_wait"));
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
