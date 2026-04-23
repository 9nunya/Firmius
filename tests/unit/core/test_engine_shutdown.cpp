#include <gtest/gtest.h>

#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "lsp/LspServerManager.hpp"
#include "lsp/LspServerRegistry.hpp"
#include "lsp/LspServerSpec.hpp"
#include "mocks/MockAgent.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::test;

namespace fs = std::filesystem;

namespace {

std::string uniqueSuffix() {
  return std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

class ScopedTempDir {
 public:
  ScopedTempDir() {
    path_ = fs::temp_directory_path() /
            ("firmius_engine_shutdown_test_" + uniqueSuffix());
    fs::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

std::string lspStubServerCommand() {
#ifndef FIRMIUS_LSP_TEST_STUB_SERVER_PATH
#error "FIRMIUS_LSP_TEST_STUB_SERVER_PATH must be defined for test_engine_shutdown"
#endif
  const fs::path exe = fs::path(FIRMIUS_LSP_TEST_STUB_SERVER_PATH);
  if (!fs::exists(exe)) {
    throw std::runtime_error("Missing lsp_test_stub_server test helper: " + exe.string());
  }
  return exe.string();
}

}  // namespace

TEST(EngineShutdownTest, InterruptsAgentsAndCancelsBlockingProcesses) {
  AgentContext context;
  context.identity.id = "agent-1";

  auto host = std::make_shared<MockHost>();
  auto environment = std::make_shared<MockEnvironment>(host);
  auto agent = std::make_shared<MockAgent>(context, environment);

  EXPECT_CALL(environment->mockProcessManager(), getBlockingProcessIds())
      .WillOnce(::testing::Return(std::vector<std::string>{"proc-1"}));
  EXPECT_CALL(environment->mockProcessManager(), killProcess("proc-1"));

  AgentRegistry::instance().registerAgent("agent-1", agent);

  Engine::instance().shutdown();

  EXPECT_TRUE(agent->wasMethodCalled("interrupt"));
  EXPECT_TRUE(
      host->wasCalledWith("killBackgroundProcess", {{"id", "proc-1"}}));

  AgentRegistry::instance().unregisterAgent("agent-1");
}

TEST(EngineShutdownTest, ShutdownDrainsLspServerManagerPool) {
  auto& manager = LspServerManager::instance();
  auto& registry = LspServerRegistry::instance();

  manager.shutdownAll();
  ASSERT_EQ(manager.activeServerCount(), 0U);

  ScopedTempDir temp;
  const fs::path workspaceRoot = temp.path() / "workspace";
  fs::create_directories(workspaceRoot);

  const std::string specId = "engine-shutdown-stub-" + uniqueSuffix();

  LspServerSpec spec;
  spec.id = specId;
  spec.extensions = {".shutdownstub"};
  spec.markers = {".git"};
  spec.commands = {{lspStubServerCommand()}};
  spec.defaultLanguageId = "plaintext";

  registry.registerCustomSpec(spec);

  const LspServerSpec* registeredSpec = registry.findById(specId);
  ASSERT_NE(registeredSpec, nullptr);

  auto* client =
      manager.getOrCreateServer(*registeredSpec, workspaceRoot.string(), 1500);
  ASSERT_NE(client, nullptr);

  EXPECT_GT(manager.activeServerCount(), 0U);

  Engine::instance().shutdown();

  EXPECT_EQ(manager.activeServerCount(), 0U);
}
