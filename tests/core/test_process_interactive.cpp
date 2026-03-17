#include "Panic.hpp"
#include "agents/Agent.hpp"
#include "environment/Environment.hpp"
#include "hosts/LocalHost.hpp"
#include "providers/NanoGPTProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include "tools/ProcessInputTool.hpp"
#include "tools/ProcessSpawnTool.hpp"
#include "tools/ProcessStatusTool.hpp"
#include "tools/ProcessWaitTool.hpp"
#include "tools/ToolRegistry.hpp"
#include "../unit/mocks/MockPermissions.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;

class ProcessInteractiveTest : public ::testing::Test {
protected:
  void SetUp() override {
    Panic::init();

    // Register a dummy provider for Agent construction
    auto &reg = ProviderRegistry::instance();
    if (!reg.getProvider("nanogpt")) {
      reg.registerProvider(std::make_shared<NanoGPTProvider>());
    }

    // Setup AgentContext
    AgentContext ctx;
    ctx.config.providerId = "nanogpt";
    ctx.config.modelId = "test-model";
    ctx.environment.type = HostType::Local;
    ctx.environment.cwd = "/tmp";
    ctx.permissions.allowedScopes = {ToolScope::Process};
    ctx.permissions.allowedPaths = {"/tmp"};

    // Registry and tools
    registry.registerTool(std::make_unique<ProcessSpawnTool>());
    registry.registerTool(std::make_unique<ProcessInputTool>());
    registry.registerTool(std::make_unique<ProcessStatusTool>());
    registry.registerTool(std::make_unique<ProcessWaitTool>());

    auto localHost = std::make_shared<LocalHost>();
    hostRaw = localHost.get();
    auto environment = std::make_shared<Environment>(
        localHost, ctx.environment.cwd,
        [](const StreamEvent & /*ev*/) {});
    auto permissions = std::make_shared<firmius::test::MockPermissions>();
    permissions->cwd_ = ctx.environment.cwd;
    permissions->allowedPaths_ = ctx.permissions.allowedPaths;
    agent = std::make_unique<Agent>(ctx, environment, permissions, registry, nullptr);
  }

  ToolRegistry registry;
  firmius::shared::IHost *hostRaw = nullptr;
  std::unique_ptr<Agent> agent;

  std::string callTool(const std::string &name, const std::string &jsonArgs) {
    rapidjson::Document doc;
    doc.Parse(jsonArgs.c_str());
    ToolContext ctx{*hostRaw, *agent, "test_call"};
    auto res = registry.execute(name, doc, ctx);
    if (!res.success) {
      throw std::runtime_error("Tool " + name + " failed: " + res.error);
    }
    return res.data;
  }
};

TEST_F(ProcessInteractiveTest, PythonInteraction) {
  // Use a stdin-driven Python loop instead of REPL mode to avoid TTY-specific
  // behavior while still verifying interactive process IO.
  std::string spawnData = callTool(
      "process_spawn",
      R"({"command": "python3 -u -c 'import sys; exec(\"for line in sys.stdin:\\n    if line.strip() == \\\"__EXIT__\\\":\\n        break\\n    sys.stdout.write(line)\\n    sys.stdout.flush()\")'"})");
  rapidjson::Document spawnDoc;
  spawnDoc.Parse(spawnData.c_str());
  std::string pid = spawnDoc["process_id"].GetString();
  EXPECT_FALSE(pid.empty());

  // 2. Send input.
  callTool("process_input",
           R"({"process_id": ")" + pid +
               R"(", "input": "firmius_interactive_test\n"})");

  // 3. Wait for the output pattern to appear.
  std::string waitData = callTool(
      "process_wait",
      R"({"process_id": ")" + pid +
          R"(", "pattern": "firmius_interactive_test", "timeout_ms": 5000})");
  rapidjson::Document waitDoc;
  waitDoc.Parse(waitData.c_str());
  EXPECT_TRUE(waitDoc["patternFound"].GetBool());
  EXPECT_TRUE(waitDoc["isRunning"].GetBool());

  // 4. Ask the script to exit.
  callTool("process_input",
           R"({"process_id": ")" + pid +
               R"(", "input": "__EXIT__\n"})");

  // 5. Verify the process exits within a bounded timeout.
  std::string exitWaitData =
      callTool("process_wait",
               R"({"process_id": ")" + pid + R"(", "timeout_ms": 5000})");
  rapidjson::Document exitWaitDoc;
  exitWaitDoc.Parse(exitWaitData.c_str());
  EXPECT_FALSE(exitWaitDoc["isRunning"].GetBool());
}

TEST_F(ProcessInteractiveTest, PatternWaitTimeout) {
  // 1. Spawn a long running process
  std::string spawnData =
      callTool("process_spawn", R"({"command": "sleep 10"})");
  rapidjson::Document spawnDoc;
  spawnDoc.Parse(spawnData.c_str());
  std::string pid = spawnDoc["process_id"].GetString();

  // 2. Wait for a pattern that will never appear with a short timeout
  // process_wait should fail if timeout is reached
  rapidjson::Document doc;
  doc.Parse(
      R"({"process_id": "placeholder", "pattern": "never", "timeout_ms": 1000})");
  doc["process_id"].SetString(pid.c_str(), doc.GetAllocator());

  ToolContext ctx{*hostRaw, *agent, "test_call"};
  auto res = registry.execute("process_wait", doc, ctx);

  EXPECT_FALSE(res.success);
  EXPECT_TRUE(res.error.find("Timeout") != std::string::npos);
}
