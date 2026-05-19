#include "Events.hpp"
#include "IAgent.hpp"
#include "tools/ToolRegistry.hpp"
#include "workflow/WorkflowLoader.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core {
namespace {

class DummyHostProcess final : public shared::IHostProcess {
public:
  void onOutput(std::function<void(const std::string &, bool)>) override {}
  shared::ProcessResult wait() override { return {}; }
  shared::ProcessSnapshot inspect() const override { return {}; }
  void kill() override {}
  void write(const std::string &) override {}
  bool isRunning() override { return false; }
  std::string getSystemId() const override { return {}; }
};

class DummyHost final : public shared::IHost {
public:
  std::string init() override { return {}; }
  void destroy() override {}
  void cleanup() override {}
  void setUser(const std::string &) override {}
  std::vector<uint8_t> readFile(const std::string &) override { return {}; }
  void writeFile(const std::string &, const std::vector<uint8_t> &) override {}
  void deleteFile(const std::string &) override {}
  bool exists(const std::string &) override { return false; }
  std::vector<shared::FileInfo> listDir(const std::string &) override { return {}; }
  shared::FileInfo stat(const std::string &) override { return {}; }
  std::string getId() const override { return "dummy-host"; }
  shared::ProcessResult exec(const std::string &, const std::string &,
                             const std::map<std::string, std::string> &,
                             std::optional<std::chrono::milliseconds>) override {
    return {};
  }
  std::unique_ptr<shared::IHostProcess>
  spawn(const std::string &, const std::string &,
        const std::map<std::string, std::string> &) override {
    return std::make_unique<DummyHostProcess>();
  }
  void registerBackgroundProcess(const std::string &,
                                 std::unique_ptr<shared::IHostProcess>) override {}
  shared::ProcessSnapshot inspectBackgroundProcess(const std::string &) override {
    return {};
  }
  void releaseBackgroundProcess(const std::string &) override {}
  void writeToBackgroundProcess(const std::string &, const std::string &) override {}
  void killBackgroundProcess(const std::string &) override {}
};

class DummyAgent final : public shared::IAgent {
public:
  DummyAgent() {
    ctx_.history = std::make_shared<shared::AgentHistory>();
    ctx_.identity.id = "agent-test";
    ctx_.identity.name = "tester";
    ctx_.config.personaName = "lead";
    ctx_.permissions.allowedScopes = {shared::ToolScope::Semantic,
                                      shared::ToolScope::FilesystemRead,
                                      shared::ToolScope::FilesystemWrite,
                                      shared::ToolScope::Process,
                                      shared::ToolScope::Delegation,
                                      shared::ToolScope::Web,
                                      shared::ToolScope::CrewRead,
                                      shared::ToolScope::CrewWrite,
                                      shared::ToolScope::CrewAssign,
                                      shared::ToolScope::CrewReview};
  }

  void reset() override {}
  void run(const std::string &, std::function<void(const shared::StreamEvent &)>,
           const std::vector<shared::ImageContent> &) override {}
  void resume(std::function<void(const shared::StreamEvent &)>) override {}
  const shared::AgentContext &getContext() const override { return ctx_; }
  shared::AgentContext &getMutableContext() override { return ctx_; }
  shared::ModelChoice getPreferredModel() const override { return {}; }
  void interrupt() override {}
  bool isInterrupted() const override { return false; }
  void clearInterrupt() override {}
  void compactNow(std::function<void(const shared::StreamEvent &)>) override {}
  void saveHistory() override {}
  void appendHistoryTurn(const shared::AgentTurn &) override {}
  void setModel(const std::string &, const std::string &) override {}
  void setModel(const std::string &, const std::string &, const std::string &) override {}
  bool isRunning() const override { return false; }
  bool isBooting() const override { return false; }
  void setBooting(bool) override {}
  std::shared_ptr<shared::IHost> getHost() override { return {}; }
  std::shared_ptr<shared::IEnvironment> getEnvironment() const override { return {}; }
  std::shared_ptr<shared::IPermissions> getPermissions() const override { return {}; }

private:
  shared::AgentContext ctx_;
};

class WorkflowDefinedToolRegistryTest : public ::testing::Test {
protected:
  std::string tempDir_;
  std::string oldWorkflowsDir_;
  bool hadWorkflowsDir_ = false;

  void SetUp() override {
    char tempTemplate[] = "/tmp/firmius_tool_registry_XXXXXX";
    char *temp = ::mkdtemp(tempTemplate);
    ASSERT_NE(temp, nullptr);
    tempDir_ = temp;

    if (const char *v = std::getenv("FIRMIUS_WORKFLOWS_DIR")) {
      oldWorkflowsDir_ = v;
      hadWorkflowsDir_ = true;
    }
    ::setenv("FIRMIUS_WORKFLOWS_DIR", tempDir_.c_str(), 1);
  }

  void TearDown() override {
    if (hadWorkflowsDir_) {
      ::setenv("FIRMIUS_WORKFLOWS_DIR", oldWorkflowsDir_.c_str(), 1);
    } else {
      ::unsetenv("FIRMIUS_WORKFLOWS_DIR");
    }
    std::filesystem::remove_all(tempDir_);
  }

  void writeWorkflow(const std::string &name, const std::string &body) {
    std::ofstream out(std::filesystem::path(tempDir_) / name);
    out << body;
  }
};

TEST_F(WorkflowDefinedToolRegistryTest, RegistersDefinedToolMetadataAndExecutes) {
  writeWorkflow(
      "promise_hook.md",
      R"(---
name: Promise Hook Victory
trigger:
  on_event: pre_tool_use
action:
  kind: compose
  steps:
    - kind: prompt
      body: promise gate
  body: promise body
defines_tool:
  name: make_pact
  description: Record a pact payload.
  required_scope: Semantic
  schema:
    type: object
    properties:
      brief: { type: string }
---
The promise hook is armed.
)");

  WorkflowLoader::instance().init();

  ToolRegistry registry;
  registry.registerWorkflowDefinedTools();

  const auto meta = registry.getMetadataFor("make_pact");
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->name, "make_pact");
  EXPECT_EQ(meta->description, "Record a pact payload.");
  EXPECT_EQ(meta->scope, shared::ToolScope::Semantic);

  rapidjson::Document input;
  input.SetObject();
  auto &alloc = input.GetAllocator();
  input.AddMember("brief", rapidjson::Value("hook platform shipped", alloc).Move(), alloc);

  DummyHost host;
  DummyAgent agent;
  shared::ToolContext ctx{host, agent, "tool-call-1", nullptr, nullptr};

  auto result = registry.execute("make_pact", input, ctx);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.data.find("make_pact"), std::string::npos);
  EXPECT_NE(result.data.find("promise_hook"), std::string::npos);
  EXPECT_NE(result.data.find("hook platform shipped"), std::string::npos);
}

} // namespace
} // namespace firmius::core
