#include "audits/SubagentStressAudit.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

std::string SubagentStressAudit::getId() const { return "subagent_stress"; }

std::string SubagentStressAudit::getDescription() const {
  return "Stress test subagent spawning";
}

shared::AuditResult
SubagentStressAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();
  std::cout << "Starting Subagent Stress Audit..." << std::endl;
  Panic::init();
  EnvLoader::load(".env.local");
  std::string provider = "antigravity";
  std::string model = "gemini-3-flash";
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--provider" && i + 1 < args.size()) {
      provider = args[++i];
    } else if (args[i] == "--model" && i + 1 < args.size()) {
      model = args[++i];
    }
  }
  auto &harness = Harness::instance();
  harness.init();
  HostCreationOptions opts;
  opts.type = HostType::Local;
  opts.containerName = "";
  opts.deleteOnExit = false;
  std::string threadId =
      harness.newThread(opts, "/home/nunya/Projects/Firmius", "general");
  if (threadId.empty()) {
    std::cerr << "Failed to create Local thread." << std::endl;
    result.exitCode = 1;
    result.passed = false;
    harness.shutdown();
    return result;
  }
  std::cout << "Local thread created: " << threadId << std::endl;
  harness.switchModel(provider, model);
  int subagentSpawns = 0;
  int subagentCompletions = 0;
  int streamErrors = 0;
  int retries = 0;
  std::string leadAgentId = harness.focusedAgentId();
  auto runTurn = [&](const std::string &prompt) -> bool {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool hadOutput = false;
    int subId = harness.subscribe([&](const AppEvent &ev) {
      std::visit(
          [&](auto &&e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, AgentError>) {
              if (e.agentId == leadAgentId) {
                std::cout << "\n[Event] AgentError (LEAD): " << e.message
                          << std::endl;
                done = true;
                cv.notify_one();
              } else {
                std::cout << "\n[Event] AgentError (SUB): " << e.message
                          << std::endl;
              }
            } else if constexpr (std::is_same_v<T, StreamError>) {
              streamErrors++;
              std::cout << "\n[Event] StreamError: " << e.message << std::endl;
            } else if constexpr (std::is_same_v<T, StreamRetrying>) {
              retries++;
            } else if constexpr (std::is_same_v<T, AgentCompleted>) {
              if (e.agentId == leadAgentId) {
                std::cout << "\n[Event] AgentCompleted (LEAD)" << std::endl;
                done = true;
                cv.notify_one();
              } else {
                std::cout << "\n[Event] AgentCompleted (SUB)" << std::endl;
                subagentCompletions++;
              }
            } else if constexpr (std::is_same_v<T, AgentSpawned>) {
              if (e.agentId != leadAgentId) {
                subagentSpawns++;
                std::cout << "\n[Event] AgentSpawned (SUB)" << std::endl;
              }
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
  std::cout << "--- Phase 1: Audit Execution ---" << std::endl;
  runTurn("Look closely at the 'packages' directory in this project. Delegate "
          "each package folder in 'packages' to be read by a separate subagent "
          "concurrently (do not wait for one to finish before starting the "
          "next). Wait for their results, then return the compiled summary of "
          "what each package does. There are 4 packages: core, provider, "
          "shared, tui.");
  std::cout << "\nAudit complete." << std::endl;
  std::cout << "Subagent Spawns: " << subagentSpawns << std::endl;
  std::cout << "Subagent Completions: " << subagentCompletions << std::endl;
  std::cout << "Stream Errors: " << streamErrors << std::endl;
  std::cout << "Provider Retries: " << retries << std::endl;
  harness.shutdown();
  if (subagentSpawns >= 2 && subagentCompletions >= 2) {
    std::cout << "\033[1;32mAUDIT PASSED\033[0m: Successfully spawned and "
                 "completed multiple subagents."
              << std::endl;
    result.exitCode = 0;
    result.passed = true;
    return result;
  }
  std::cout << "\033[1;31mAUDIT FAILED\033[0m: Did not spawn and complete "
               "sufficient subagents."
            << std::endl;
  result.exitCode = 1;
  result.passed = false;
  return result;
}

} // namespace firmius::audits
