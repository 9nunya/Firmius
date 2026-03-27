#include "audits/LoopCancellationAudit.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "AgentRegistry.hpp"
#include "harness/Harness.hpp"
#include <condition_variable>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

std::string LoopCancellationAudit::getId() const {
  return "loop_cancellation";
}

std::string LoopCancellationAudit::getDescription() const {
  return "Replicate core loop cancellation issue";
}

shared::AuditResult LoopCancellationAudit::run(const std::vector<std::string> &) {
  AuditResult result;
  result.auditId = getId();
  std::cout << "Starting Loop Cancellation Audit..." << std::endl;
  Panic::init();
  EnvLoader::load(".env.local");
  auto &harness = Harness::instance();
  harness.init();
  harness.debugLogging = true;

  HostCreationOptions opts;
  opts.type = HostType::Docker;
  opts.deleteOnExit = true;

  std::string threadId = harness.newThread(opts, "/work", "lead");
  if (threadId.empty()) {
    std::cerr << "[Audit] Failed to create thread." << std::endl;
    result.passed = false;
    result.exitCode = 1;
    return result;
  }

  std::cout << "[Audit] Preparing files in host..." << std::endl;
  // Trigger agent creation by sending an initial prompt or explicitly summoning
  harness.send("Hello"); // This will create the lead agent
  std::string agentId;
  std::shared_ptr<IAgent> agent;
  for (int i = 0; i < 50; ++i) {
      agentId = harness.focusedAgentId();
      if (!agentId.empty()) {
          agent = AgentRegistry::instance().getAgent(agentId);
          if (agent) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!agent) {
      std::cerr << "[Audit] Could not find focused agent in registry." << std::endl;
      result.passed = false;
      result.exitCode = 1;
      return result;
  }

  auto host = agent->getHost();
  if (!host) {
      std::cerr << "[Audit] Agent has no host." << std::endl;
      result.passed = false;
      result.exitCode = 1;
      return result;
  }

  host->exec("mkdir -p /work");
  host->exec("sh -c 'echo \"apple\" > /work/1.txt'");
  host->exec("sh -c 'echo \"banana\" > /work/2.txt'");
  host->exec("sh -c 'echo \"cherry\" > /work/3.txt'");

  harness.switchModel("antigravity", "gemini-3-flash", "max");

  auto runUntilActivityAndMaybeCancel = [&](const std::string &prompt, bool cancelOnActivity) {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool activityDetected = false;
    int subId = harness.subscribe([&](const AppEvent &ev) {
      std::visit([&](auto &&e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, AgentText> || std::is_same_v<T, AgentThinking> || std::is_same_v<T, AgentToolCall>) {
          {
            std::lock_guard<std::mutex> lk(mtx);
            if (!activityDetected) {
              activityDetected = true;
              if (cancelOnActivity) {
                std::cout << "[Audit] Activity detected, cancelling..." << std::endl;
                harness.abort();
              }
            }
          }
        } else if constexpr (std::is_same_v<T, AgentTurnCompleted> || std::is_same_v<T, AgentError>) {
          std::lock_guard<std::mutex> lk(mtx);
          done = true;
          cv.notify_one();
        }
      }, ev);
    });

    std::cout << "\n> Prompt: " << prompt << std::endl;
    harness.send(prompt);

    std::unique_lock<std::mutex> lk(mtx);
    if (!cv.wait_for(lk, std::chrono::seconds(60), [&] { return done; })) {
        std::cout << "[Audit] Timeout waiting for turn completion." << std::endl;
    }
    harness.unsubscribe(subId);
  };

  std::cout << "--- Phase 1: Start and Cancel ---" << std::endl;
  runUntilActivityAndMaybeCancel("Write a very long technical explanation of C++ templates.", true);

  std::this_thread::sleep_for(std::chrono::seconds(2));

  std::cout << "--- Phase 2: Multi-turn Task ---" << std::endl;
  int turnCount = 0;
  int subId = harness.subscribe([&](const AppEvent &ev) {
      if (std::holds_alternative<AgentTurnCompleted>(ev)) {
          turnCount++;
          std::cout << "[Audit] Turn " << turnCount << " completed." << std::endl;
      }
  });

  runUntilActivityAndMaybeCancel("List all .txt files in /work. Then read each one and tell me their contents. You MUST read all three files.", false);
  harness.unsubscribe(subId);

  std::cout << "[Audit] Total turns in Phase 2: " << turnCount << std::endl;

  harness.shutdown();
  
  result.passed = (turnCount > 1);
  result.exitCode = result.passed ? 0 : 1;
  return result;
}

} // namespace firmius::audits
