#include <gtest/gtest.h>

#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "lsp/LspServerManager.hpp"
#include "lsp/LspServerRegistry.hpp"
#include "lsp/LspServerSpec.hpp"
#include "mocks/MockAgent.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
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

std::string makeStubServerScript(const fs::path& scriptPath) {
  const std::string script = R"SCRIPT(#!/usr/bin/env bash
set -euo pipefail

echo "stub-started $$" >&2

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
  if [[ "${body}" =~ \"method\":\"([^\"]+)\" ]]; then
    method="${BASH_REMATCH[1]}"
  fi
  if [[ "${body}" =~ \"id\":([0-9]+) ]]; then
    req_id="${BASH_REMATCH[1]}"
  fi

  if [[ "${method}" == "initialize" ]]; then
    response="{\"jsonrpc\":\"2.0\",\"id\":${req_id},\"result\":{\"capabilities\":{}}}"
    printf 'Content-Length: %d\r\n\r\n%s' "${#response}" "${response}"
  elif [[ "${method}" == "shutdown" ]]; then
    response="{\"jsonrpc\":\"2.0\",\"id\":${req_id},\"result\":null}"
    printf 'Content-Length: %d\r\n\r\n%s' "${#response}" "${response}"
  elif [[ "${method}" == "exit" ]]; then
    exit 0
  fi
done
)SCRIPT";

  std::ofstream out(scriptPath);
  out << script;
  out.close();

  fs::permissions(scriptPath,
                  fs::perms::owner_read | fs::perms::owner_write |
                      fs::perms::owner_exec | fs::perms::group_read |
                      fs::perms::group_exec | fs::perms::others_read |
                      fs::perms::others_exec,
                  fs::perm_options::replace);

  return scriptPath.string();
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
  const fs::path scriptPath = temp.path() / "stub_lsp_server.sh";
  const fs::path workspaceRoot = temp.path() / "workspace";
  fs::create_directories(workspaceRoot);

  const std::string specId = "engine-shutdown-stub-" + uniqueSuffix();

  LspServerSpec spec;
  spec.id = specId;
  spec.extensions = {".shutdownstub"};
  spec.markers = {".git"};
  spec.commands = {{makeStubServerScript(scriptPath)}};
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
