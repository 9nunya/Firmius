#include "Events.hpp"
#include "IAgent.hpp"
#include "ITool.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "agents/PurposeLoader.hpp"
#include "hosts/LocalHost.hpp"
#include "tools/ReadTool.hpp"
#include "tools/ListTool.hpp"
#include "tools/GrepTool.hpp"
#include "tools/GlobTool.hpp"
#include "tools/FileEditTool.hpp"
#include "tools/LspDiagnosticsTool.hpp"
#include "tools/LspTool.hpp"
#include "tools/ProcessTool.hpp"
#include "tools/PythonExecuteTool.hpp"
#include "tools/SkillLoadTool.hpp"
#include "tools/ToolRegistry.hpp"
#include "utils/Hashline.hpp"
#include "ConfigLoader.hpp"
#include "AgentRegistry.hpp"
#include "lsp/LspServerManager.hpp"
#include "lsp/LspServerRegistry.hpp"
#include "lsp/LspServerSpec.hpp"
#include "mcp/McpClient.hpp"
#include "mcp/McpManager.hpp"
#include "mcp/McpStdioSession.hpp"
#include "mcp/McpHttpSession.hpp"
#include <filesystem>
#include <fstream>
#include <gmock/gmock.h>
#include <cstdlib>
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <set>
#include <atomic>
#include <thread>
#include <tuple>
#include <optional>
#include <cstdlib>

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
  MOCK_METHOD(void, deleteFile, (const std::string &), (override));
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
  MOCK_METHOD(void, releaseBackgroundProcess, (const std::string &),
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
  MOCK_METHOD(ModelChoice, getPreferredModel, (), (const, override));
  MOCK_METHOD(void, interrupt, (), (override));
  MOCK_METHOD(bool, isInterrupted, (), (const, override));
  MOCK_METHOD(void, clearInterrupt, (), (override));
  MOCK_METHOD(void, compactNow,
              (std::function<void(const StreamEvent &)>), (override));
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

rapidjson::Document createMultiFileEditJson(
    const std::vector<std::pair<std::string,
                                std::vector<std::map<std::string, std::string>>>> &files) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();

  rapidjson::Value fileArray(rapidjson::kArrayType);
  for (const auto &[path, edits] : files) {
    rapidjson::Value fileObj(rapidjson::kObjectType);
    fileObj.AddMember("path", makeJsonString(path, alloc), alloc);

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
        editObj.AddMember(makeJsonString(key, alloc),
                          makeJsonString(value, alloc), alloc);
      }
      if (!newLines.Empty()) {
        editObj.AddMember("new_lines", newLines, alloc);
      }
      editArray.PushBack(editObj, alloc);
    }

    fileObj.AddMember("edits", editArray, alloc);
    fileArray.PushBack(fileObj, alloc);
  }

  doc.AddMember("files", fileArray, alloc);
  return doc;
}

rapidjson::Document createSearchReplaceFileEditJson(
    const std::string &path,
    const std::vector<std::tuple<std::string, std::string, bool>> &replacements) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("path", makeJsonString(path, alloc), alloc);

  rapidjson::Value editArray(rapidjson::kArrayType);
  for (const auto &[oldString, newString, replaceAll] : replacements) {
    rapidjson::Value editObj(rapidjson::kObjectType);
    editObj.AddMember("op", makeJsonString("search_replace", alloc), alloc);
    editObj.AddMember("old_string", makeJsonString(oldString, alloc), alloc);
    editObj.AddMember("new_string", makeJsonString(newString, alloc), alloc);
    editObj.AddMember("replace_all", replaceAll, alloc);
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

void addEmptyFileEditModeNoise(rapidjson::Document &doc) {
  auto &alloc = doc.GetAllocator();
  doc.AddMember("content", makeJsonString("", alloc), alloc);
  doc.AddMember("patch", makeJsonString("", alloc), alloc);
  addFileEditLegacyNoise(doc, "", "", false, 0.0f);
}

std::string lspTestUniqueSuffix() {
  return std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

class ScopedLspTestDir {
public:
  ScopedLspTestDir() {
    path_ = std::filesystem::temp_directory_path() /
            ("firmius_tools_lsp_test_" + lspTestUniqueSuffix());
    std::filesystem::create_directories(path_);
  }

  ~ScopedLspTestDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::string writeDeterministicLspStub(const std::filesystem::path &scriptPath) {
  const std::string script = R"SCRIPT(#!/usr/bin/env bash
set -euo pipefail

send_msg() {
  local payload="$1"
  printf 'Content-Length: %d\r\n\r\n%s' "${#payload}" "${payload}"
}

emit_diagnostics() {
  local body="$1"
  local uri="$2"
  local diagnostics='[]'

  if [[ "${body}" == *"triple_error_warning"* ]]; then
    diagnostics='[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"severity":1,"message":"first fail"},{"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}},"severity":1,"message":"second fail"},{"range":{"start":{"line":2,"character":0},"end":{"line":2,"character":1}},"severity":2,"message":"be careful"}]'
  elif [[ "${body}" == *"beta"* ]]; then
    diagnostics='[{"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":4}},"severity":1,"message":"broken call"},{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"severity":2,"message":"weak type"}]'
  fi

  local notification="{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":\"${uri}\",\"diagnostics\":${diagnostics}}}"
  send_msg "${notification}"
}

while true; do
  content_length=""
  while IFS= read -r line; do
    line="${line%$'\r'}"
    [[ -z "${line}" ]] && break
    if [[ "${line}" == Content-Length:* ]]; then
      content_length="${line#Content-Length: }"
    fi
  done || exit 0

  [[ -z "${content_length}" ]] && continue
  IFS= read -r -N "${content_length}" body || exit 0

  method=""
  req_id="null"
  req_uri="file:///dev/null"
  if [[ "${body}" =~ \"method\":\"([^\"]+)\" ]]; then
    method="${BASH_REMATCH[1]}"
  fi
  if [[ "${body}" =~ \"id\":([0-9]+) ]]; then
    req_id="${BASH_REMATCH[1]}"
  fi
  if [[ "${body}" =~ \"uri\":\"([^\"]+)\" ]]; then
    req_uri="${BASH_REMATCH[1]}"
  fi

  if [[ "${method}" == "initialize" ]]; then
    response="{\"jsonrpc\":\"2.0\",\"id\":${req_id},\"result\":{\"capabilities\":{\"referencesProvider\":true}}}"
    send_msg "${response}"
  elif [[ "${method}" == "shutdown" ]]; then
    response="{\"jsonrpc\":\"2.0\",\"id\":${req_id},\"result\":null}"
    send_msg "${response}"
  elif [[ "${method}" == "exit" ]]; then
    exit 0
  elif [[ "${method}" == "textDocument/didOpen" || "${method}" == "textDocument/didChange" ]]; then
    emit_diagnostics "${body}" "${req_uri}"
  elif [[ "${method}" == "textDocument/references" ]]; then
    response="{\"jsonrpc\":\"2.0\",\"id\":${req_id},\"result\":[{\"uri\":\"${req_uri}\",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":5}}}]}"
    send_msg "${response}"
  elif [[ "${req_id}" != "null" ]]; then
    response="{\"jsonrpc\":\"2.0\",\"id\":${req_id},\"result\":null}"
    send_msg "${response}"
  fi
done
)SCRIPT";

  std::ofstream out(scriptPath);
  out << script;
  out.close();

  std::filesystem::permissions(
      scriptPath,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
          std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
          std::filesystem::perms::others_exec,
      std::filesystem::perm_options::replace);

  return scriptPath.string();
}

class StubLspHarness {
public:
  StubLspHarness()
      : serverId_("tools-stub-" + lspTestUniqueSuffix()),
        extension_(".lspstub" + lspTestUniqueSuffix()) {
    workspace_ = tempDir_.path() / "workspace";
    scriptPath_ = tempDir_.path() / "stub_lsp.sh";
    std::filesystem::create_directories(workspace_);

    LspServerSpec spec;
    spec.id = serverId_;
    spec.defaultLanguageId = "plaintext";
    spec.extensions = {extension_};
    spec.commands = {{writeDeterministicLspStub(scriptPath_)}};
    LspServerRegistry::instance().registerCustomSpec(std::move(spec));
  }

  ~StubLspHarness() {
    LspServerManager::instance().shutdownServer(serverId_, workspace_.string());
  }

  std::string serverId() const { return serverId_; }
  std::string filePath(const std::string &stem) const {
    return (workspace_ / (stem + extension_)).string();
  }

private:
  ScopedLspTestDir tempDir_;
  std::string serverId_;
  std::string extension_;
  std::filesystem::path workspace_;
  std::filesystem::path scriptPath_;
};
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
  ReadTool tool;
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

  ToolResult executeRead(const rapidjson::Value &json, ToolContext &ctx) {
    rapidjson::Document doc;
    doc.CopyFrom(json, doc.GetAllocator());
    return tool.execute(doc, ctx);
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

  auto result = executeRead(json, ctx);

  EXPECT_TRUE(result.success);
  rapidjson::Document resultDoc;
  resultDoc.Parse(result.data.c_str());
  auto expectedContent =
      std::string("1|Line 1\n2|Line 2\n3|Line 3");
  ASSERT_TRUE(resultDoc.HasMember("content"));
  ASSERT_TRUE(resultDoc["content"].IsString());
  EXPECT_EQ(std::string(resultDoc["content"].GetString()), expectedContent);
  // Token-waste pass 5: line_start/line_end/lines_read/read_full/
  // reached_end/watch_scope/watch_state collapsed into one `range:
  // "<start>-<end>/<total>"` field.
  ASSERT_TRUE(resultDoc.HasMember("range"));
  EXPECT_EQ(std::string(resultDoc["range"].GetString()), "1-3/3");
}

TEST_F(FileReadToolTest, allowedPaths_blocksOutside) {
  mockAgent.defaultCtx.permissions.allowedPaths = {};
  mockAgent.defaultCtx.permissions.allowOutsideCwd = false;

  mockAgent.mockPerms_->allowOutsideCwd_ = false;
  mockAgent.mockPerms_->allowedPaths_ = {};

  auto json = createJsonInput({{"path", "/etc/passwd"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto result = executeRead(json, ctx);

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

  auto result = executeRead(json, ctx);

  EXPECT_TRUE(result.success);
  rapidjson::Document resultDoc;
  resultDoc.Parse(result.data.c_str());
  auto expectedContent =
      std::string("2|Line 2\n3|Line 3\n4|Line 4");
  ASSERT_TRUE(resultDoc.HasMember("content"));
  ASSERT_TRUE(resultDoc["content"].IsString());
  EXPECT_EQ(std::string(resultDoc["content"].GetString()), expectedContent);
  // Token-waste pass 5: range collapses line_start/line_end/lines_read/
  // reached_end/watch_scope into a "<start>-<end>/<total>" string.
  ASSERT_TRUE(resultDoc.HasMember("range"));
  EXPECT_EQ(std::string(resultDoc["range"].GetString()), "2-4/5");
}

TEST_F(FileReadToolTest, fileReadLoadsNearestAgentsWithoutReloadingRoot) {
  std::filesystem::create_directories("/tmp/work");
  const std::filesystem::path rootAgentsPath = "/tmp/work/AGENTS.md";
  const std::filesystem::path nestedRoot = "/tmp/work/agents_file_read_test";
  const std::filesystem::path packageDir = nestedRoot / "pkg";
  const std::filesystem::path moduleDir = packageDir / "module";
  const std::filesystem::path packageAgentsPath = packageDir / "AGENTS.md";
  const std::filesystem::path moduleAgentsPath = moduleDir / "AGENTS.md";
  const std::filesystem::path targetFilePath = moduleDir / "target.txt";

  std::filesystem::remove_all(nestedRoot);
  std::filesystem::remove(rootAgentsPath);
  std::filesystem::create_directories(moduleDir);

  {
    std::ofstream rootAgents(rootAgentsPath);
    rootAgents << "root guidance";
  }
  {
    std::ofstream packageAgents(packageAgentsPath);
    packageAgents << "package guidance";
  }
  {
    std::ofstream moduleAgents(moduleAgentsPath);
    moduleAgents << "module guidance";
  }
  {
    std::ofstream targetFile(targetFilePath);
    targetFile << "hello\n";
  }

  ASSERT_TRUE(PurposeLoader::loadProjectRootAgentsIntoContext(
      mockAgent.defaultCtx));

  auto json =
      createJsonInput({{"path", "agents_file_read_test/pkg/module/target.txt"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto result = executeRead(json, ctx);
  EXPECT_TRUE(result.success) << result.error;

  const std::string canonicalRootAgents =
      std::filesystem::weakly_canonical(rootAgentsPath).string();
  const std::string canonicalPackageAgents =
      std::filesystem::weakly_canonical(packageAgentsPath).string();
  const std::string canonicalModuleAgents =
      std::filesystem::weakly_canonical(moduleAgentsPath).string();

  EXPECT_EQ(std::count(mockAgent.defaultCtx.state.loadedAgentMds.begin(),
                       mockAgent.defaultCtx.state.loadedAgentMds.end(),
                       canonicalRootAgents),
            1);
  EXPECT_EQ(std::count(mockAgent.defaultCtx.state.loadedAgentMds.begin(),
                       mockAgent.defaultCtx.state.loadedAgentMds.end(),
                       canonicalPackageAgents),
            1);
  EXPECT_EQ(std::count(mockAgent.defaultCtx.state.loadedAgentMds.begin(),
                       mockAgent.defaultCtx.state.loadedAgentMds.end(),
                       canonicalModuleAgents),
            1);

  std::filesystem::remove_all(nestedRoot);
  std::filesystem::remove(rootAgentsPath);
}

class SkillLoadToolTest : public ::testing::Test {
protected:
  SkillLoadTool tool;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  std::filesystem::path tempRoot;
  std::filesystem::path skillsRoot;
  std::optional<std::string> previousSkillsDir;

  void SetUp() override {
    tempRoot = "/tmp/firmius_skill_load_tool_test";
    skillsRoot = tempRoot / "skills";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(skillsRoot);

    if (const char *existing = std::getenv("FIRMIUS_SKILLS_DIR")) {
      previousSkillsDir = std::string(existing);
    }

    setenv("FIRMIUS_SKILLS_DIR", skillsRoot.string().c_str(), 1);

    mockAgent.defaultCtx.environment.cwd = "/tmp/work";
    mockAgent.defaultCtx.permissions.allowOutsideCwd = false;
    mockAgent.defaultCtx.permissions.allowedPaths = {skillsRoot.string(),
                                                     "/tmp/work", "/tmp"};

    mockAgent.mockPerms_->allowOutsideCwd_ = false;
    mockAgent.mockPerms_->allowedPaths_ = {skillsRoot.string(), "/tmp/work",
                                           "/tmp"};
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
    ON_CALL(mockHost, readFile(_))
        .WillByDefault(Invoke([](const std::string &path) {
          std::ifstream file(path, std::ios::binary);
          if (!file.is_open()) {
            return std::vector<uint8_t>{};
          }
          return std::vector<uint8_t>(
              (std::istreambuf_iterator<char>(file)),
              std::istreambuf_iterator<char>());
        }));
  }

  void TearDown() override {
    if (previousSkillsDir.has_value()) {
      setenv("FIRMIUS_SKILLS_DIR", previousSkillsDir->c_str(), 1);
    } else {
      unsetenv("FIRMIUS_SKILLS_DIR");
    }
    std::filesystem::remove_all(tempRoot);
  }

  static rapidjson::Document parseResult(const ToolResult &result) {
    rapidjson::Document doc;
    doc.Parse(result.data.c_str());
    return doc;
  }
};

TEST_F(SkillLoadToolTest, loadsTopLevelSkillEntrypoint) {
  const std::filesystem::path skillDir = skillsRoot / "planner";
  std::filesystem::create_directories(skillDir);
  std::ofstream skillFile(skillDir / "SKILL.md");
  skillFile << "---\n";
  skillFile << "name: Planner Skill\n";
  skillFile << "description: Planning helper\n";
  skillFile << "---\n";
  skillFile << "Use this skill for planning tasks.\n";
  skillFile.close();

  auto json = createJsonInput({{"what", "planner"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success) << result.error;
  auto doc = parseResult(result);
  ASSERT_TRUE(doc.IsObject());
  EXPECT_STREQ(doc["skill_id"].GetString(), "planner");
  EXPECT_STREQ(doc["name"].GetString(), "Planner Skill");
  EXPECT_STREQ(doc["description"].GetString(), "Planning helper");
  EXPECT_STREQ(doc["relative_path"].GetString(), "SKILL.md");
  EXPECT_NE(std::string(doc["path"].GetString()).find("/planner/SKILL.md"),
            std::string::npos);
  EXPECT_NE(std::string(doc["content"].GetString()).find("Use this skill"),
            std::string::npos);
}

TEST_F(SkillLoadToolTest, loadsNestedSkillReferencePath) {
  const std::filesystem::path skillDir = skillsRoot / "reviewer";
  const std::filesystem::path refsDir = skillDir / "references";
  std::filesystem::create_directories(refsDir);

  std::ofstream skillFile(skillDir / "SKILL.md");
  skillFile << "---\nname: Reviewer\ndescription: Review helper\n---\nBody\n";
  skillFile.close();

  std::ofstream referenceFile(refsDir / "checklist.md");
  referenceFile << "- item 1\n- item 2\n";
  referenceFile.close();

  auto json =
      createJsonInput({{"what", "reviewer/references/checklist.md"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success) << result.error;
  auto doc = parseResult(result);
  ASSERT_TRUE(doc.IsObject());
  EXPECT_STREQ(doc["skill_id"].GetString(), "reviewer");
  EXPECT_STREQ(doc["relative_path"].GetString(),
               "references/checklist.md");
  EXPECT_NE(std::string(doc["path"].GetString())
                .find("/reviewer/references/checklist.md"),
            std::string::npos);
  EXPECT_NE(std::string(doc["content"].GetString()).find("item 2"),
            std::string::npos);
}

TEST_F(SkillLoadToolTest, rejectsTraversalOutsideSkillRoot) {
  const std::filesystem::path skillDir = skillsRoot / "writer";
  std::filesystem::create_directories(skillDir);

  std::ofstream skillFile(skillDir / "SKILL.md");
  skillFile << "---\nname: Writer\n---\nBody\n";
  skillFile.close();

  auto json = createJsonInput({{"what", "writer/../outside.md"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("cannot traverse outside"), std::string::npos);
}

TEST_F(SkillLoadToolTest, rejectsSymlinkEscapingSkillRoot) {
  const std::filesystem::path skillDir = skillsRoot / "attacker";
  std::filesystem::create_directories(skillDir);

  std::ofstream skillFile(skillDir / "SKILL.md");
  skillFile << "---\nname: Attacker\n---\nBody\n";
  skillFile.close();

  const std::filesystem::path secretsFile = tempRoot / "secrets.txt";
  std::ofstream secret(secretsFile);
  secret << "sensitive information\n";
  secret.close();

  // Create a symlink inside the skill root pointing outside
  std::error_code ec;
  std::filesystem::create_symlink(secretsFile, skillDir / "link_to_secrets.txt", ec);
  if (ec) {
    GTEST_SKIP() << "Symlinks not supported in this environment: " << ec.message();
  }

  auto json = createJsonInput({{"what", "attacker/link_to_secrets.txt"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("stay within the skill root"), std::string::npos);
}


TEST_F(SkillLoadToolTest, allowsSymlinkWithinSkillRoot) {
  const std::filesystem::path skillDir = skillsRoot / "honest";
  const std::filesystem::path dataDir = skillDir / "data";
  std::filesystem::create_directories(dataDir);

  std::ofstream skillFile(skillDir / "SKILL.md");
  skillFile << "---\nname: Honest\n---\nBody\n";
  skillFile.close();

  const std::filesystem::path internalFile = dataDir / "internal.txt";
  std::ofstream internal(internalFile);
  internal << "internal data\n";
  internal.close();

  // Create a symlink inside the skill root pointing inside
  std::error_code ec;
  std::filesystem::create_symlink(internalFile, skillDir / "link_to_internal.txt", ec);
  if (ec) {
    GTEST_SKIP() << "Symlinks not supported in this environment: " << ec.message();
  }

  auto json = createJsonInput({{"what", "honest/link_to_internal.txt"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_TRUE(result.success) << result.error;
  auto doc = parseResult(result);
  EXPECT_STREQ(doc["relative_path"].GetString(), "link_to_internal.txt");
  EXPECT_NE(std::string(doc["content"].GetString()).find("internal data"),
            std::string::npos);
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

  ToolResult executeGlob(const rapidjson::Value &json, ToolContext &ctx) {
    rapidjson::Document doc;
    doc.CopyFrom(json, doc.GetAllocator());
    return tool.execute(doc, ctx);
  }
};

TEST_F(GlobToolTest, noMatchesReturnsEmptyArray) {
  addEntry("/tmp", true);
  addEntry("/tmp/file.cpp", false);

  auto json = createJsonInput({{"pattern", "*.nonexistent"}, {"path", "/tmp"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto toolResult = executeGlob(json, ctx);

  EXPECT_TRUE(toolResult.success);
  // Token-waste pass 3: prose-first; no-matches result is the prose
  // "no matches under <path>".
  EXPECT_NE(toolResult.data.find("no matches"), std::string::npos);
  EXPECT_NE(toolResult.data.find("\"count\":0"), std::string::npos);
}

TEST_F(GlobToolTest, listDirFailureReturnsError) {
  addEntry("/root", true);
  EXPECT_CALL(mockHost, listDir("/root"))
      .WillOnce(::testing::Throw(std::runtime_error("Permission denied")));

  auto json = createJsonInput({{"pattern", "*.txt"}, {"path", "/root"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto toolResult = executeGlob(json, ctx);

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

  auto toolResult = executeGlob(json, ctx);

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

  auto toolResult = executeGlob(json, ctx);

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

  ToolResult executeGrep(const rapidjson::Value &json, ToolContext &ctx) {
    rapidjson::Document doc;
    doc.CopyFrom(json, doc.GetAllocator());
    return tool.execute(doc, ctx);
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

  executeGrep(json, ctx);
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

  auto toolResult = executeGrep(json, ctx);

  EXPECT_TRUE(toolResult.success);
  // Token-waste pass 3: prose-first; no-matches result is the prose
  // "no matches" with hits/files counters at 0.
  EXPECT_NE(toolResult.data.find("no matches"), std::string::npos);
  EXPECT_NE(toolResult.data.find("\"hits\":0"), std::string::npos);
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
                               {"path", "/tmp"}},
                              {{"context_before", 1}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto toolResult = executeGrep(json, ctx);

  EXPECT_TRUE(toolResult.success);
  // Token-waste pass 3: prose-first. With context lines requested, each
  // hit appears as `<path>:<line>:<content>` (match) or `-` (context).
  EXPECT_NE(toolResult.data.find("/tmp/dir:with:colon/file.cpp"),
            std::string::npos);
  EXPECT_NE(toolResult.data.find(":6:int value = 42;"), std::string::npos);
  EXPECT_NE(toolResult.data.find("-5-// before"), std::string::npos);
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

  auto toolResult = executeGrep(json, ctx);

  EXPECT_TRUE(toolResult.success);
  // Token-waste pass 3: with no context_before/after the prose lists
  // file (count): line, line, ... — line number 7 should appear.
  EXPECT_NE(toolResult.data.find("7"), std::string::npos);
  EXPECT_NE(toolResult.data.find("/tmp/file.txt"), std::string::npos);
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


class FileToolFamilyTest : public ::testing::Test {
protected:
  ToolRegistry registry;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  std::string capturedWrite;

  void SetUp() override {
    registry.registerTool(std::make_unique<FileEditTool>());
    registry.registerTool(std::make_unique<FileWriteTool>());
    registry.registerTool(std::make_unique<FileReplaceTool>());
    registry.registerTool(std::make_unique<FileRangeTool>());

    mockAgent.defaultCtx.environment.cwd = "/tmp/work";
    mockAgent.defaultCtx.permissions.allowOutsideCwd = false;
    mockAgent.defaultCtx.permissions.allowedPaths = {"/tmp/work", "/tmp"};
    mockAgent.defaultCtx.permissions.allowedScopes = {ToolScope::FilesystemWrite};
    mockAgent.mockPerms_->allowOutsideCwd_ = false;
    mockAgent.mockPerms_->allowedPaths_ = {"/tmp/work", "/tmp"};
    mockAgent.mockPerms_->cwd_ = "/tmp/work";

    ON_CALL(mockAgent, getContext()).WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent, getMutableContext()).WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), resolvePath(_))
        .WillByDefault(Invoke([](const std::string &path) {
          if (path.starts_with("/")) return path;
          return "/tmp/work/" + path;
        }));
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), hasFullyReadFile(_))
        .WillByDefault(Return(true));
    ON_CALL(mockAgent.mockEnv_->mockWorkspace(), getCurrentWorkingDirectory())
        .WillByDefault(Return("/tmp/work"));
    ON_CALL(mockHost, writeFile(_, _))
        .WillByDefault(Invoke([this](const std::string &, const std::vector<uint8_t> &data) {
          capturedWrite.assign(data.begin(), data.end());
        }));
  }

  static std::vector<uint8_t> bytes(const std::string &text) {
    return std::vector<uint8_t>(text.begin(), text.end());
  }

  static std::string anchor(int line, const std::string &) { return std::to_string(line); }

  static rapidjson::Document parseResult(const ToolResult &result) {
    rapidjson::Document doc;
    doc.Parse(result.data.c_str());
    return doc;
  }

  ToolResult executeNamed(const char *toolName, rapidjson::Document &input) {
    ToolContext ctx{mockHost, mockAgent, "test_call"};
    return registry.execute(toolName, input, ctx);
  }
};

TEST_F(CommandPermissionToolTest,
       fileEdit_deniedWriteApprovalDoesNotWriteFile) {
  FileWriteTool tool;
  mockAgent.mockPerms_->editApprovalResponse_ = PermissionResponse::Deny;

  auto json = createJsonInput({{"path", "blocked.txt"}, {"content", "new content"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  ITool *itool = &tool;
  auto result = itool->execute(json, ctx);

  EXPECT_FALSE(result.success);
  ASSERT_EQ(mockAgent.mockPerms_->requestedEditPaths_.size(), 1u);
  EXPECT_EQ(mockAgent.mockPerms_->requestedEditPaths_[0], "/tmp/work/blocked.txt");
}

TEST_F(FileToolFamilyTest, editAppliesSingleFilePatchWithHeaders) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\ngamma\n";

  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember(
      "patch",
      makeJsonString("--- a/file.txt\n+++ b/file.txt\n@@ -2,1 +2,2 @@\n-beta\n+beta2\n+gamma2", alloc),
      alloc);

  auto result = executeNamed("Edit", input);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ(capturedWrite, "alpha\nbeta2\ngamma2\ngamma\n");
  auto doc = parseResult(result);
  // Token-waste pass 1: result is prose-first; verb confirms patch path.
  EXPECT_THAT(std::string(doc["result"].GetString()), HasSubstr("Patched"));
  EXPECT_THAT(std::string(doc["result"].GetString()), HasSubstr("file.txt"));
}

TEST_F(FileToolFamilyTest, editRejectsPatchWithoutUnifiedHeaders) {
  rapidjson::Document input;
  input.SetObject();
  input.AddMember("patch", makeJsonString("@@ -1,1 +1,1 @@\n-a\n+b", input.GetAllocator()),
                  input.GetAllocator());

  auto result = executeNamed("Edit", input);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("unified diff"));
}

TEST_F(FileToolFamilyTest, editValidateOnlyDoesNotWrite) {
  const std::string path = "/tmp/work/file.txt";
  const std::string original = "alpha\nbeta\n";
  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes(original)));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("patch",
                  makeJsonString("--- a/file.txt\n+++ b/file.txt\n@@ -2,1 +2,1 @@\n-beta\n+beta2", alloc),
                  alloc);
  input.AddMember("validate_only", true, alloc);

  auto result = executeNamed("Edit", input);
  EXPECT_TRUE(result.success) << result.error;
  auto doc = parseResult(result);
  EXPECT_TRUE(doc["validate_only"].GetBool());
  EXPECT_THAT(std::string(doc["result"].GetString()), HasSubstr("Validated"));
}

TEST_F(FileToolFamilyTest, editTransactionalFailurePreventsPartialWrites) {
  const std::string firstPath = "/tmp/work/a.txt";
  const std::string secondPath = "/tmp/work/b.txt";
  EXPECT_CALL(mockHost, exists(firstPath)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(firstPath)).WillOnce(Return(bytes("one\n")));
  EXPECT_CALL(mockHost, exists(secondPath)).WillOnce(Return(false));
  EXPECT_CALL(mockHost, writeFile(_, _)).Times(0);

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("patch",
                  makeJsonString("--- a/a.txt\n+++ b/a.txt\n@@ -1,1 +1,1 @@\n-one\n+two\n--- a/b.txt\n+++ b/b.txt\n@@ -1,1 +1,1 @@\n-old\n+new", alloc),
                  alloc);

  auto result = executeNamed("Edit", input);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("existing file"));
}

TEST_F(FileToolFamilyTest, editPatchFailureStaysPatchNative) {
  const std::string path = "/tmp/work/file.txt";
  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes("alpha\nbeta\n")));

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("patch",
                  makeJsonString("--- a/file.txt\n+++ b/file.txt\n@@ -99,1 +99,1 @@\n-old\n+new",
                                 alloc),
                  alloc);

  auto result = executeNamed("Edit", input);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("\"resolved_mode\":\"patch\""));
  EXPECT_THAT(result.error, HasSubstr("Patch hunk context not found"));
  EXPECT_THAT(result.error, HasSubstr("regenerate the patch"));
}

TEST_F(FileToolFamilyTest, editWriteOverwritesFile) {
  const std::string path = "/tmp/work/file.txt";
  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes("old\n")));

  auto input = createJsonInput({{"path", "file.txt"}, {"content", "new\nbody\n"}});
  auto result = executeNamed("EditWrite", input);

  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ(capturedWrite, "new\nbody\n");
  auto doc = parseResult(result);
  // Token-waste pass 1: prose verb identifies the EditWrite path.
  EXPECT_THAT(std::string(doc["result"].GetString()), HasSubstr("Wrote"));
}

TEST_F(FileToolFamilyTest, editReplaceAppliesReplaceAll) {
  const std::string path = "/tmp/work/file.txt";
  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes("alpha beta\nbeta alpha\n")));

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("path", makeJsonString("file.txt", alloc), alloc);
  rapidjson::Value replacements(rapidjson::kArrayType);
  rapidjson::Value replacement(rapidjson::kObjectType);
  replacement.AddMember("old_string", makeJsonString("alpha", alloc), alloc);
  replacement.AddMember("new_string", makeJsonString("omega", alloc), alloc);
  replacement.AddMember("replace_all", true, alloc);
  replacements.PushBack(replacement, alloc);
  input.AddMember("replacements", replacements, alloc);

  auto result = executeNamed("EditReplace", input);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ(capturedWrite, "omega beta\nbeta omega\n");
  auto doc = parseResult(result);
  // Token-waste pass 1: replacement count is reported in the prose `result`.
  EXPECT_THAT(std::string(doc["result"].GetString()),
              HasSubstr("2 replacements"));
}

TEST_F(FileToolFamilyTest, editRangeReplacesAnchoredLinesAndReportsSanitation) {
  const std::string path = "/tmp/work/file.txt";
  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes("keep-a\nbeta\ngamma\nkeep-b\n")));

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("path", makeJsonString("file.txt", alloc), alloc);
  rapidjson::Value operations(rapidjson::kArrayType);
  rapidjson::Value op(rapidjson::kObjectType);
  op.AddMember("op", makeJsonString("replace_range", alloc), alloc);
  op.AddMember("start_anchor", makeJsonString(anchor(2, "beta"), alloc), alloc);
  op.AddMember("end_anchor", makeJsonString(anchor(3, "gamma"), alloc), alloc);
  rapidjson::Value newLines(rapidjson::kArrayType);
  newLines.PushBack(makeJsonString("keep-a", alloc), alloc);
  newLines.PushBack(makeJsonString("2#8c72|+ beta2", alloc), alloc);
  newLines.PushBack(makeJsonString("3#f2c5|48a8|- gamma2", alloc), alloc);
  newLines.PushBack(makeJsonString("keep-b", alloc), alloc);
  op.AddMember("new_lines", newLines, alloc);
  operations.PushBack(op, alloc);
  input.AddMember("operations", operations, alloc);

  auto result = executeNamed("EditRange", input);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ(capturedWrite, "keep-a\nbeta2\ngamma2\nkeep-b\n");
  auto doc = parseResult(result);
  // Token-waste pass 1: range verb confirms the EditRange path; sanitation
  // notes are reported inline in the prose `result` (the input contained
  // line-range / hashline prefixes that the trimmer removed).
  EXPECT_THAT(std::string(doc["result"].GetString()),
              HasSubstr("Edited (range)"));
  EXPECT_THAT(std::string(doc["result"].GetString()), HasSubstr("stripped"));
}

TEST_F(FileToolFamilyTest, editRangeRejectsMissingEndAnchor) {
  const std::string path = "/tmp/work/file.txt";
  EXPECT_CALL(mockHost, exists(path)).WillOnce(Return(true));
  EXPECT_CALL(mockHost, readFile(path)).WillOnce(Return(bytes("alpha\nbeta\n")));

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("path", makeJsonString("file.txt", alloc), alloc);
  rapidjson::Value operations(rapidjson::kArrayType);
  rapidjson::Value op(rapidjson::kObjectType);
  op.AddMember("op", makeJsonString("replace_range", alloc), alloc);
  op.AddMember("start_anchor", makeJsonString("1", alloc), alloc);
  rapidjson::Value newLines(rapidjson::kArrayType);
  newLines.PushBack(makeJsonString("x", alloc), alloc);
  op.AddMember("new_lines", newLines, alloc);
  operations.PushBack(op, alloc);
  input.AddMember("operations", operations, alloc);

  auto result = executeNamed("EditRange", input);
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("end_anchor"));
}

class ProcessExecuteToolTest : public ::testing::Test {
protected:
  ProcessTool tool;
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
  }

  ToolResult executeProcess(const rapidjson::Value &json, ToolContext &ctx,
                            const char *action) {
    rapidjson::Document doc;
    doc.CopyFrom(json, doc.GetAllocator());
    doc.AddMember("action",
                  rapidjson::Value(action, doc.GetAllocator()).Move(),
                  doc.GetAllocator());
    return tool.execute(doc, ctx);
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
      .WillRepeatedly(Return(ProcessSnapshot{false, 0, "output", "", 100.0, ""}));

  auto json = createJsonInput({{"command", "echo test"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  executeProcess(json, ctx, "Execute");

  EXPECT_EQ(capturedCwd, "/tmp/work");
}

TEST_F(ProcessExecuteToolTest, timeoutHandling) {
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _, _)).WillOnce(Return("proc_123"));

  ProcessSnapshot runningSnapshot{true, -1, "partial output", "", 100.0, ""};
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), inspectProcess(_))
      .WillRepeatedly(Return(runningSnapshot));

  auto json = createJsonInput({{"command", "sleep 100"}}, {{"timeout_ms", 50}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto result = executeProcess(json, ctx, "Execute");

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("Timeout"), std::string::npos);
}

TEST_F(ProcessExecuteToolTest, blocksForeignApplyPatchCommand) {
  auto json = createJsonInput({{"command", "apply_patch <<'PATCH'\n*** Begin Patch\n*** End Patch\nPATCH"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto result = executeProcess(json, ctx, "Execute");

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("Files (Read) + Edit (patch) instead"));
}

TEST_F(ProcessExecuteToolTest, nonZeroExitReturnsFailureWithStructuredResult) {
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), spawnProcess(_, _, _, _, _))
      .WillOnce(Return("proc_123"));
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(), inspectProcess(_))
      .WillRepeatedly(Return(ProcessSnapshot{false, 17, "out", "err", 10.0, ""}));

  auto json = createJsonInput({{"command", "false"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto result = executeProcess(json, ctx, "Execute");

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("non-zero exit code: 17"));
  // Token-waste pass 2: result is prose-first; exit_code stays as a
  // structured field but command_success was dropped (derivable from
  // exit_code).
  EXPECT_THAT(result.data, HasSubstr("\"exit_code\":17"));
  EXPECT_THAT(result.data, HasSubstr("Exited 17"));
}

class ProcessStatusToolTest : public ::testing::Test {
protected:
  ProcessTool tool;
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;

  void SetUp() override {
    ON_CALL(mockAgent, getContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
    ON_CALL(mockAgent, getMutableContext())
        .WillByDefault(ReturnRef(mockAgent.defaultCtx));
  }

  ToolResult executeProcess(const rapidjson::Value &json, ToolContext &ctx, const char *action) {
    rapidjson::Document doc;
    doc.CopyFrom(json, doc.GetAllocator());
    doc.AddMember("action", rapidjson::Value(action, doc.GetAllocator()).Move(), doc.GetAllocator());
    return tool.execute(doc, ctx);
  }
};

TEST_F(ProcessStatusToolTest, rejectsAgentIdWithActionableHint) {
  auto otherAgent = std::make_shared<NiceMock<MockAgent>>();
  AgentRegistry::instance().registerAgent("agent-123", otherAgent);

  auto json = createJsonInput({{"process_id", "agent-123"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};
  auto result = executeProcess(json, ctx, "Status");

  AgentRegistry::instance().unregisterAgent("agent-123");

  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.error, HasSubstr("belongs to a subagent"));
  EXPECT_THAT(result.error, HasSubstr("Delegate.wait"));
}

class ListDirectoryToolTest : public ::testing::Test {
protected:
  ListTool tool;
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

  ToolResult executeList(const rapidjson::Value &json, ToolContext &ctx) {
    rapidjson::Document doc;
    doc.CopyFrom(json, doc.GetAllocator());
    return tool.execute(doc, ctx);
  }
};

TEST_F(ListDirectoryToolTest, allowedPaths_enforced) {
  mockAgent.defaultCtx.permissions.allowedPaths = {};
  mockAgent.defaultCtx.permissions.allowOutsideCwd = false;

  mockAgent.mockPerms_->allowOutsideCwd_ = false;
  mockAgent.mockPerms_->allowedPaths_ = {};

  auto json = createJsonInput({{"path", "/etc"}});
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto result = executeList(json, ctx);
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

  auto result = executeList(json, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.data.find(".hidden"), std::string::npos);
  EXPECT_NE(result.data.find("visible"), std::string::npos);
}

class PythonExecuteToolTest : public ::testing::Test {
protected:
  PythonExecuteTool tool;
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
  }
};

TEST_F(PythonExecuteToolTest, schemaIncludesOptionalVenv) {
  auto schema = tool.getSchema();
  ASSERT_NE(schema, nullptr);
  const std::string json = schema->toString();
  EXPECT_NE(json.find("\"venv\""), std::string::npos);
}

TEST_F(PythonExecuteToolTest, usesProvidedVenvInterpreterAndRequestsReadAccess) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("code", rapidjson::Value("print('hi')\n", alloc).Move(), alloc);
  doc.AddMember("venv", rapidjson::Value("/opt/project/.venv", alloc).Move(), alloc);

  ProcessSnapshot finished;
  finished.running = false;
  finished.exitCode = 0;
  finished.stdoutData = "hi\n";
  finished.stderrData = "";
  finished.elapsedMs = 3;

  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(),
              spawnProcess(testing::HasSubstr("/opt/project/.venv/bin/python -c"),
                           testing::_, "/tmp/work", testing::_, false))
      .WillOnce(testing::Return("proc-venv"));
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(),
              addBlockingProcessId("proc-venv"));
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(),
              inspectProcess("proc-venv"))
      .WillOnce(testing::Return(finished));
  EXPECT_CALL(mockAgent.mockEnv_->mockProcessManager(),
              removeBlockingProcessId("proc-venv"));

  ToolContext ctx{mockHost, mockAgent, "test_call"};
  ITool *itool = &tool;
  auto result = itool->execute(doc, ctx);

  EXPECT_TRUE(result.success) << result.error;
  ASSERT_GE(mockAgent.mockPerms_->requestedEditPaths_.size(), 1u);
  EXPECT_EQ(mockAgent.mockPerms_->requestedEditPaths_.front(),
            "/opt/project/.venv");
  ASSERT_GE(mockAgent.mockPerms_->requestedCommands_.size(), 1u);
  EXPECT_NE(mockAgent.mockPerms_->requestedCommands_.front().find(
                "/opt/project/.venv/bin/python -c"),
            std::string::npos);
  // Token-waste pass 2: PythonExecuteTool result no longer echoes the
  // venv path back (the model just sent it). The above checks on
  // requestedEditPaths_ / requestedCommands_ already prove the venv
  // was honoured.
}

TEST_F(PythonExecuteToolTest, realExecutionCanUseProvidedVenv) {
  const char *venvPath = std::getenv("FIRMIUS_TEST_REAL_PYTHON_VENV");
  if (venvPath == nullptr || std::string(venvPath).empty()) {
    GTEST_SKIP() << "FIRMIUS_TEST_REAL_PYTHON_VENV not set";
  }

  LocalHost localHost;
  localHost.init();

  mockAgent.defaultCtx.permissions.allowedPaths.push_back(std::string(venvPath));
  mockAgent.mockPerms_->allowedPaths_.push_back(std::string(venvPath));

  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("code", rapidjson::Value("import sys\nprint(sys.executable)\n", alloc).Move(), alloc);
  doc.AddMember("venv", rapidjson::Value(venvPath, alloc).Move(), alloc);

  ToolContext ctx{localHost, mockAgent, "test_call"};
  ITool *itool = &tool;
  auto result = itool->execute(doc, ctx);

  EXPECT_TRUE(result.success) << result.error;
  EXPECT_NE(result.data.find(std::string(venvPath) + "/bin/python"),
            std::string::npos);
  localHost.destroy();
}

namespace {

std::string frameMcpJson(const std::string &json) {
  return "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
}

class StubMcpSession final : public mcp::IMcpSession {
public:
  rapidjson::Document sendRequest(int, const std::string &method,
                                  const rapidjson::Value &, int,
                                  const std::string &,
                                  ToolContext * = nullptr) override {
    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();
    doc.AddMember("jsonrpc", "2.0", a);
    doc.AddMember("id", 1, a);
    rapidjson::Value result(rapidjson::kObjectType);
    if (method == "initialize") {
      rapidjson::Value capabilities(rapidjson::kObjectType);
      capabilities.AddMember("tools", rapidjson::Value(rapidjson::kObjectType),
                             a);
      result.AddMember("capabilities", capabilities, a);
    }
    doc.AddMember("result", result, a);
    return doc;
  }

  void sendNotification(const std::string &,
                        const rapidjson::Value &) override {}
};

} // namespace

TEST(McpManagerTest, GetOrCreateSharesSingleClientForConcurrentRequests) {
  mcp::McpManager manager;
  std::atomic<int> factoryCalls{0};
  std::vector<std::shared_ptr<mcp::McpClient>> clients(8);
  std::vector<std::thread> threads;

  for (size_t i = 0; i < clients.size(); ++i) {
    threads.emplace_back([&, i]() {
      clients[i] = manager.getOrCreateClient("server1", [&]() {
        factoryCalls.fetch_add(1, std::memory_order_relaxed);
        return std::make_shared<mcp::McpClient>(
            std::make_unique<StubMcpSession>());
      });
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }

  ASSERT_NE(clients.front(), nullptr);
  for (const auto &client : clients) {
    EXPECT_EQ(client, clients.front());
  }
  EXPECT_EQ(factoryCalls.load(), 1);
  EXPECT_EQ(manager.clientCountForTest(), 1u);

  manager.shutdown();
  EXPECT_EQ(manager.clientCountForTest(), 0u);
}

TEST(McpClientSubstrateTest, initializeListCallAndShutdownLifecycle) {
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto mockProcess = std::make_unique<NiceMock<MockHostProcess>>();
  auto *process = mockProcess.get();

  bool running = true;
  std::string stdoutBuffer;
  std::vector<std::string> writes;

  ON_CALL(*process, inspect())
      .WillByDefault(Invoke([&]() {
        return ProcessSnapshot{running, 0, stdoutBuffer, "", 0.0, ""};
      }));
  ON_CALL(*process, isRunning()).WillByDefault(Invoke([&]() { return running; }));

  EXPECT_CALL(*process, kill()).WillRepeatedly(Invoke([&]() { running = false; }));
  EXPECT_CALL(*process, write(_)).WillRepeatedly(Invoke([&](const std::string &payload) {
    writes.push_back(payload);
    if (payload.find("\"method\":\"initialize\"") != std::string::npos) {
      stdoutBuffer += frameMcpJson(
          R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{"tools":{}}}})");
    } else if (payload.find("\"method\":\"tools/list\"") != std::string::npos) {
      stdoutBuffer += frameMcpJson(
          R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"echo_tool"}]}})");
    } else if (payload.find("\"method\":\"tools/call\"") != std::string::npos) {
      stdoutBuffer += frameMcpJson(
          R"({"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"ok"}]}})");
    } else if (payload.find("\"method\":\"shutdown\"") != std::string::npos) {
      stdoutBuffer += frameMcpJson(
          R"({"jsonrpc":"2.0","id":4,"result":{}})");
    }
  }));

  {
    mcp::McpClient client(std::move(mockProcess));
    client.initialize(1000);

    auto listed = client.listTools(1000);
    ASSERT_TRUE(listed.HasMember("result"));
    ASSERT_TRUE(listed["result"].HasMember("tools"));
    ASSERT_EQ(listed["result"]["tools"].GetArray().Size(), 1u);

    rapidjson::Document args;
    args.SetObject();
    args.AddMember("input", "hello", args.GetAllocator());
    auto called = client.callTool("echo_tool", args, 1000);
    ASSERT_TRUE(called.HasMember("result"));
  }

  ASSERT_GE(writes.size(), 4u);
  EXPECT_THAT(writes[0], HasSubstr("\"method\":\"initialize\""));
  EXPECT_THAT(writes[1], HasSubstr("\"method\":\"notifications/initialized\""));
  EXPECT_THAT(writes[2], HasSubstr("\"method\":\"tools/list\""));
  EXPECT_THAT(writes[3], HasSubstr("\"method\":\"tools/call\""));
  if (writes.size() >= 6u) {
    EXPECT_THAT(writes[4], HasSubstr("\"method\":\"shutdown\""));
    EXPECT_THAT(writes[5], HasSubstr("\"method\":\"exit\""));
  }
}

TEST(McpClientSubstrateTest, initializeFailsWhenCapabilitiesToolsMissing) {
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto mockProcess = std::make_unique<NiceMock<MockHostProcess>>();
  auto *process = mockProcess.get();

  bool running = true;
  std::string stdoutBuffer;
  ON_CALL(*process, inspect())
      .WillByDefault(Invoke([&]() {
        return ProcessSnapshot{running, 0, stdoutBuffer, "", 0.0, ""};
      }));
  EXPECT_CALL(*process, kill()).WillRepeatedly(Invoke([&]() { running = false; }));
  EXPECT_CALL(*process, write(_)).WillRepeatedly(Invoke([&](const std::string &payload) {
    if (payload.find("\"method\":\"initialize\"") != std::string::npos) {
      stdoutBuffer += frameMcpJson(
          R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}})");
    }
  }));

  mcp::McpClient client(std::move(mockProcess));
  EXPECT_THROW(client.initialize(1000), std::runtime_error);
}

TEST(McpClientSubstrateTest, stdioSessionTimeoutKillsProcess) {
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  auto mockProcess = std::make_unique<NiceMock<MockHostProcess>>();
  auto *process = mockProcess.get();

  bool running = true;
  ON_CALL(*process, inspect())
      .WillByDefault(Invoke([&]() {
        return ProcessSnapshot{running, 0, "", "", 0.0, ""};
      }));
  EXPECT_CALL(*process, write(_)).Times(1);
  EXPECT_CALL(*process, kill()).WillRepeatedly(Invoke([&]() { running = false; }));

  mcp::McpStdioSession session(*process);
  rapidjson::Document params;
  params.SetObject();

  EXPECT_THROW(
      session.sendRequest(1, "initialize", params, 30, "initialize"),
      std::runtime_error);
}

TEST(McpClientSubstrateTest, httpSessionInitializeListCallAndShutdownLifecycle) {
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  std::vector<std::string> methods;
  mcp::McpHttpSender sender = [&](const mcp::McpHttpTransportConfig &,
                                  const std::string &requestBody, int) {
    rapidjson::Document req;
    req.Parse(requestBody.c_str());
    methods.push_back(req["method"].GetString());

    const std::string method = req["method"].GetString();
    if (method == "initialize") {
      return mcp::McpHttpResponse{
          200,
          R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{"tools":{}}}})",
          "",
          {}};
    }
    if (method == "tools/list") {
      return mcp::McpHttpResponse{
          200,
          R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"echo_tool"}]}})",
          "",
          {}};
    }
    if (method == "tools/call") {
      return mcp::McpHttpResponse{
          200,
          R"({"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"ok"}]}})",
          "",
          {}};
    }
    if (method == "shutdown") {
      return mcp::McpHttpResponse{200, R"({"jsonrpc":"2.0","id":4,"result":{}})", "", {}};
    }
    return mcp::McpHttpResponse{200, R"({"jsonrpc":"2.0","result":{}})", "", {}};
  };

  mcp::McpHttpTransportConfig httpConfig;
  httpConfig.url = "https://example.invalid/mcp";
  auto session = std::make_unique<mcp::McpHttpSession>(httpConfig, sender);

  {
    mcp::McpClient client(std::move(session));
    client.initialize(1000);
    auto listed = client.listTools(1000);
    ASSERT_TRUE(listed.HasMember("result"));
    ASSERT_TRUE(listed["result"].HasMember("tools"));
    ASSERT_EQ(listed["result"]["tools"].GetArray().Size(), 1u);

    rapidjson::Document args;
    args.SetObject();
    args.AddMember("input", "hello", args.GetAllocator());
    auto called = client.callTool("echo_tool", args, 1000);
    ASSERT_TRUE(called.HasMember("result"));
  }

  ASSERT_GE(methods.size(), 4u);
  EXPECT_EQ(methods[0], "initialize");
  EXPECT_EQ(methods[1], "notifications/initialized");
  EXPECT_EQ(methods[2], "tools/list");
  EXPECT_EQ(methods[3], "tools/call");
  if (methods.size() >= 6u) {
    EXPECT_EQ(methods[4], "shutdown");
    EXPECT_EQ(methods[5], "exit");
  }
  EXPECT_THAT(methods, ::testing::Not(::testing::Contains(std::string("$/cancelRequest"))));
  EXPECT_THAT(methods, ::testing::Not(::testing::Contains(std::string("notifications/cancelled"))));
}

TEST(McpClientSubstrateTest, httpSessionSurfacesSenderError) {
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  mcp::McpHttpSender sender = [](const mcp::McpHttpTransportConfig &,
                                  const std::string &, int) {
    return mcp::McpHttpResponse{0, "", "auth rejected", {}};
  };

  mcp::McpHttpTransportConfig httpConfig;
  httpConfig.url = "https://example.invalid/mcp";
  auto session = std::make_unique<mcp::McpHttpSession>(httpConfig, sender);
  mcp::McpClient client(std::move(session));

  try {
    client.initialize(1000);
    FAIL() << "Expected initialize to throw";
  } catch (const std::runtime_error &e) {
    EXPECT_THAT(std::string(e.what()), HasSubstr("auth rejected"));
  }
}

TEST(McpClientSubstrateTest, httpSessionRejectsSseStreamResponseAsDeferred) {
  NiceMock<MockHost> mockHost;
  NiceMock<MockAgent> mockAgent;
  ToolContext ctx{mockHost, mockAgent, "test_call"};

  mcp::McpHttpSender sender = [](const mcp::McpHttpTransportConfig &,
                                  const std::string &, int) {
    return mcp::McpHttpResponse{
        200,
        "event: message\n"
        "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n",
        "",
        {{"content-type", "text/event-stream"}}};
  };

  mcp::McpHttpTransportConfig httpConfig;
  httpConfig.url = "https://example.invalid/mcp";
  auto session = std::make_unique<mcp::McpHttpSession>(httpConfig, sender);
  mcp::McpClient client(std::move(session));

  try {
    client.initialize(1000);
    FAIL() << "Expected initialize to throw for SSE stream response";
  } catch (const std::runtime_error &e) {
    EXPECT_THAT(std::string(e.what()), HasSubstr("invalid JSON"));
  }
}

// Legacy MCP wrapper-tool tests removed: repo now validates MCP via direct substrate tests above.
// The live MCP surface here is exercised through mcp::McpClient / McpStdioSession / McpHttpSession.
// Wrapper-style Mcp*Tool tests targeted deleted classes and no longer match the repository surface.
