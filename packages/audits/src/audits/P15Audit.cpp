#include "audits/P15Audit.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "harness/Harness.hpp"
#include <iostream>
#include <string>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

std::string P15Audit::getId() const { return "p15"; }

std::string P15Audit::getDescription() const {
  return "Persistent subagent communication audit";
}

shared::AuditResult P15Audit::run(const std::vector<std::string> &) {
  AuditResult result;
  result.auditId = getId();
  Panic::init();
  EnvLoader::load(".env.local");
  std::cout << "=== P15 Persistent Subagent Communication Audit ==="
            << std::endl;
  std::cout << std::endl;
  auto &harness = Harness::instance();
  harness.init();
  HostCreationOptions opts;
  opts.type = HostType::Local;
  opts.containerName = "";
  opts.connectToExisting = false;
  opts.deleteOnExit = false;
  auto outcomeKindToString = [](const AgentOutcome::Kind kind) {
    switch (kind) {
    case AgentOutcome::Kind::Response:
      return "Response";
    case AgentOutcome::Kind::NoSummary:
      return "NoSummary";
    case AgentOutcome::Kind::Cancelled:
      return "Cancelled";
    case AgentOutcome::Kind::Failed:
      return "Failed";
    }
    return "Unknown";
  };
  auto logOutcome = [&](const std::string &label,
                        const std::optional<AgentOutcome> &outcome) {
    if (!outcome.has_value()) {
      std::cerr << "  -> " << label << " outcome: Timeout" << std::endl;
      return false;
    }
    std::cout << "  -> " << label << " outcome: "
              << outcomeKindToString(outcome->kind) << " | "
              << outcome->text.substr(0, 100)
              << (outcome->text.size() > 100 ? "..." : "") << std::endl;
    return outcome->kind == AgentOutcome::Kind::Response ||
           outcome->kind == AgentOutcome::Kind::NoSummary;
  };
  int exitCode = 0;
  do {
    std::string threadId = harness.newThread(opts, "/tmp", "researcher");
    if (threadId.empty()) {
      std::cerr << "Failed to create thread" << std::endl;
      exitCode = 1;
      break;
    }
    std::cout << "[TEST 1] Creating new subagent via summonAgent..."
              << std::endl;
    std::string persona = "researcher";
    std::string task1 = "Say 'hello'.";
    std::string agentId =
        Engine::instance().summonAgent(threadId, persona, task1, false);
    std::cout << "  -> Agent created with ID: " << agentId << std::endl;
    std::cout << "  -> Waiting for agent to complete..." << std::endl;
    auto optResult1 = Engine::instance().waitForAgentOutcome(agentId);
    if (!logOutcome("Task 1", optResult1)) {
      std::cerr << "  -> Task 1 did not complete successfully" << std::endl;
      exitCode = 1;
      break;
    }
    std::cout << std::endl;
    std::cout << "[TEST 2] Re-tasking existing agent via executeTask..."
              << std::endl;
    std::string task2 = "Create a file /tmp/p15_test.txt with content 'Hello "
                        "from P15' and confirm";
    Engine::instance().executeTask(agentId, task2);
    std::cout << "  -> Re-tasking agent " << agentId << std::endl;
    auto optResult2 = Engine::instance().waitForAgentOutcome(agentId);
    if (!logOutcome("Task 2", optResult2)) {
      std::cerr << "  -> Task 2 did not complete successfully" << std::endl;
      exitCode = 1;
      break;
    }
    std::cout << std::endl;
    std::cout << "[TEST 3] Verifying agent persistence in registry..."
              << std::endl;
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (agent) {
      std::cout << "  -> Agent found in registry: YES" << std::endl;
      std::cout << "  -> Agent thread ID: "
                << agent->getContext().history->threadId << std::endl;
      std::cout << "  -> History turns count: "
                << agent->getContext().history->turns.size() << std::endl;
    } else {
      std::cout << "  -> Agent found in registry: NO (FAIL)" << std::endl;
      exitCode = 1;
      break;
    }
    std::cout << std::endl;
    std::cout << "[TEST 4] Terminating agent via terminateAgent..."
              << std::endl;
    Engine::instance().terminateAgent(agentId);
    std::cout << "  -> Terminate called for agent " << agentId << std::endl;
    auto agentAfter = AgentRegistry::instance().getAgent(agentId);
    if (!agentAfter) {
      std::cout << "  -> Agent removed from registry: YES" << std::endl;
    } else {
      std::cout << "  -> Agent removed from registry: NO (FAIL)" << std::endl;
      exitCode = 1;
      break;
    }
    std::cout << std::endl;
    std::cout << "[TEST 5] Testing SubagentTool with agent_id for re-tasking..."
              << std::endl;
    std::string agentId2 = Engine::instance().summonAgent(
        threadId, "lead", "Return 'initial task done'", false);
    std::cout << "  -> Created second agent: " << agentId2 << std::endl;
    auto optResult3 = Engine::instance().waitForAgentOutcome(agentId2);
    if (!logOutcome("Task 5", optResult3)) {
      std::cerr << "  -> Task 5 did not complete successfully" << std::endl;
      exitCode = 1;
      break;
    }
    std::cout << "  -> First task completed" << std::endl;
    std::cout << std::endl;
    std::cout << "[TEST 6] Listing active agents..." << std::endl;
    auto activeAgents = Engine::instance().listActiveAgents();
    std::cout << "  -> Active agents count: " << activeAgents.size()
              << std::endl;
    for (const auto &id : activeAgents) {
      std::cout << "     - " << id << std::endl;
    }
    std::cout << std::endl;
    if (activeAgents.empty()) {
      std::cerr << "FAIL: activeAgents is empty! Agent likely crashed."
                << std::endl;
      exitCode = 1;
      break;
    }
    std::cout << "[TEST 7] Resuming AGAIN - trying to resume the first "
                 "available agent"
              << std::endl;
    Engine::instance().executeTask(
        activeAgents.front(), "If you remember doing a task, please output "
                              "directly what was originally tasked for you.");
    auto result3 = Engine::instance().waitForAgentOutcome(activeAgents.front());
    if (!logOutcome("Task 7", result3)) {
      std::cerr << "  -> Task 7 did not complete successfully" << std::endl;
      exitCode = 1;
      break;
    }
    std::cout << "[TEST 8] Cleanup - terminating remaining agents..."
              << std::endl;
    for (const auto &id : activeAgents) {
      Engine::instance().terminateAgent(id);
      std::cout << "  -> Terminated: " << id << std::endl;
    }
    std::cout << std::endl;
    std::cout << "=== P15 Audit Complete ===" << std::endl;
    std::cout << "All tests passed!" << std::endl;
  } while (false);
  result.exitCode = exitCode;
  result.passed = (exitCode == 0);
  harness.shutdown();
  return result;
}

} // namespace firmius::audits
