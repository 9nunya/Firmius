#include "Events.hpp"
#include "harness/Harness.hpp"
#include <EnvLoader.hpp>
#include <Panic.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

using namespace firmius::core;
using namespace firmius::shared;
using namespace std::chrono_literals;

int main() {
  std::cout << "Starting Antigravity Permissions Audit..." << std::endl;
  Panic::init();
  EnvLoader::load(".env.local");

  // Enable pretty printing for better visibility of tool calls/results
  setenv("FIRMIUS_PRETTY_PRINT", "1", 1);

  // Smoke-skip when Docker is missing — the audit is a live run that
  // requires both Docker and a real LLM API key. CI shouldn't hang.
  if (std::system("docker info >/dev/null 2>&1") != 0) {
    std::cout << "Docker not available — skipping audit." << std::endl;
    return 0;
  }

  auto &harness = Harness::instance();
  harness.init();

  // Auto-allow every permission escalation that arrives during the audit
  // — the audit's purpose is to exercise the agent loop, not to test
  // interactive permission UX. Without this the harness blocks
  // forever on the escalation CV.
  int permSubId = harness.subscribe([&](const AppEvent &ev) {
    if (auto *req = std::get_if<PermissionEscalationRequest>(&ev)) {
      std::cout << "[Audit] Auto-allowing permission: " << req->category
                << " " << (req->command.empty() ? req->targetPath
                                                 : req->command) << std::endl;
      harness.resolvePermissionEscalation(req->requestId,
                                          PermissionResponse::AllowOnce);
    }
  });

  HostCreationOptions opts;
  opts.type = HostType::Docker;
  opts.containerName = "antigravity-permissions-sandbox";
  opts.deleteOnExit = true;

  std::string threadId = harness.newThread(opts, "/work", "lead");
  if (threadId.size() == 0) {
    std::cerr << "Failed to create Docker thread." << std::endl;
    return 1;
  }
  std::cout << "Docker thread created: " << threadId << std::endl;

  harness.switchModel("antigravity", "gemini-3-flash");

  auto runTurn = [&](const std::string &prompt) -> bool {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool hadOutput = false;
    std::string agentResponse;

    int subId = harness.subscribe([&](const AppEvent &ev) {
      std::visit(
          [&](auto &&e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, AgentError>) {
              std::cout << "[Event] AgentError: " << e.message << std::endl;
              done = true;
              cv.notify_one();
            } else if constexpr (std::is_same_v<T, AgentTurnCompleted>) {
              std::cout << "[Event] AgentTurnCompleted" << std::endl;
              done = true;
              cv.notify_one();
            } else if constexpr (std::is_same_v<T, AgentText>) {
              std::cout << e.delta << std::flush;
              agentResponse += e.delta;
              hadOutput = true;
            } else if constexpr (std::is_same_v<T, AgentThinking>) {
              std::cout << "\x1B[3m" << e.delta << "\x1B[0m" << std::flush;
              hadOutput = true;
            } else if constexpr (std::is_same_v<T, AgentToolCall>) {
              std::cout << "\n[Tool Call] " << e.toolName << "(" << e.toolArgs
                        << ")" << std::endl;
            } else if constexpr (std::is_same_v<T, AgentProcessSpawned>) {
              std::cout << "[Process Spawned] " << e.command << std::endl;
            } else if constexpr (std::is_same_v<T, AgentProcessOutput>) {
              std::cout << e.output << std::flush;
            }
          },
          ev);
    });

    std::cout << "\n> User: " << prompt << std::endl;
    harness.send(prompt);

    std::unique_lock<std::mutex> lk(mtx);
    cv.wait(lk, [&] { return done; });
    harness.unsubscribe(subId);
    std::cout << std::endl;
    return hadOutput;
  };

  std::cout << "--- Phase 1: Host-Side Environment Setup ---" << std::endl;
  // Use runTurn for warmup to ensure we wait for it.
  runTurn("Prepare to audit.");

  std::cout << "Creating audit environment via docker exec..." << std::endl;
  std::string setupCmd = "docker exec antigravity-permissions-sandbox sh -c '"
                         "mkdir -p /work/audit_dir1 && "
                         "echo \"TOKEN_1_1\" > /work/audit_dir1/file1.txt'";

  if (system(setupCmd.c_str()) != 0) {
    std::cerr << "Failed to set up environment via docker exec." << std::endl;
    return 1;
  }

  std::cout << "--- Phase 2: Agent Verification ---" << std::endl;
  runTurn("List the contents of the root directory '/' using list_directory or "
          "run_command which should fail due to path constraints. Do not give "
          "up if it fails.");

  std::cout << "\nAudit complete." << std::endl;

  harness.unsubscribe(permSubId);
  harness.shutdown();
  return 0;
}
