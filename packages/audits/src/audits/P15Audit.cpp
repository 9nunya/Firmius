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

std::string P15Audit::getDescription() const { return "Persistent subagent communication audit"; }

shared::AuditResult P15Audit::run(const std::vector<std::string>&) {
    AuditResult result;
    result.auditId = getId();
    Panic::init();
    EnvLoader::load(".env.local");
    std::cout << "=== P15 Persistent Subagent Communication Audit ===" << std::endl;
    std::cout << std::endl;
    auto& harness = Harness::instance();
    harness.init();
    HostCreationOptions opts;
    opts.type = HostType::Local;
    opts.containerName = "";
    opts.connectToExisting = false;
    opts.deleteOnExit = false;
    int exitCode = 0;
    do {
        std::string threadId = harness.newThread(opts, "/tmp", "researcher");
        if (threadId.empty()) {
            std::cerr << "Failed to create thread" << std::endl;
            exitCode = 1;
            break;
        }
        std::cout << "[TEST 1] Creating new subagent via summonAgent..." << std::endl;
        std::string persona = "researcher";
        std::string task1 = "Say 'hello'.";
        std::string agentId = Engine::instance().summonAgent(threadId, persona, task1, false);
        std::cout << "  -> Agent created with ID: " << agentId << std::endl;
        std::cout << "  -> Waiting for agent to complete..." << std::endl;
        auto optResult1 = Engine::instance().waitForAgent(agentId);
        std::string result1 = optResult1.value_or("Timeout");
        std::cout << "  -> Task 1 result: " << result1.substr(0, 100) << (result1.size() > 100 ? "..." : "") << std::endl;
        std::cout << std::endl;
        std::cout << "[TEST 2] Re-tasking existing agent via executeTask..." << std::endl;
        std::string task2 = "Create a file /tmp/p15_test.txt with content 'Hello from P15' and confirm";
        Engine::instance().executeTask(agentId, task2);
        std::cout << "  -> Re-tasking agent " << agentId << std::endl;
        auto optResult2 = Engine::instance().waitForAgent(agentId);
        std::string result2 = optResult2.value_or("Timeout");
        std::cout << "  -> Task 2 result: " << result2.substr(0, 100) << (result2.size() > 100 ? "..." : "") << std::endl;
        std::cout << std::endl;
        std::cout << "[TEST 3] Verifying agent persistence in registry..." << std::endl;
        auto agent = AgentRegistry::instance().getAgent(agentId);
        if (agent) {
            std::cout << "  -> Agent found in registry: YES" << std::endl;
            std::cout << "  -> Agent thread ID: " << agent->getContext().history->threadId << std::endl;
            std::cout << "  -> History turns count: " << agent->getContext().history->turns.size() << std::endl;
        } else {
            std::cout << "  -> Agent found in registry: NO (FAIL)" << std::endl;
            exitCode = 1;
            break;
        }
        std::cout << std::endl;
        std::cout << "[TEST 4] Terminating agent via terminateAgent..." << std::endl;
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
        std::cout << "[TEST 5] Testing SubagentTool with agent_id for re-tasking..." << std::endl;
        std::string agentId2 = Engine::instance().summonAgent(threadId, "coder", "Return 'initial task done'", false);
        std::cout << "  -> Created second agent: " << agentId2 << std::endl;
        Engine::instance().waitForAgent(agentId2);
        std::cout << "  -> First task completed" << std::endl;
        std::cout << std::endl;
        std::cout << "[TEST 6] Listing active agents..." << std::endl;
        auto activeAgents = Engine::instance().listActiveAgents();
        std::cout << "  -> Active agents count: " << activeAgents.size() << std::endl;
        for (const auto& id : activeAgents) {
            std::cout << "     - " << id << std::endl;
        }
        std::cout << std::endl;
        std::cout << "[TEST 7] Resuming AGAIN - trying to resume the first available agent" << std::endl;
        Engine::instance().executeTask(activeAgents.front(), "If you remember doing a task, please output directly what was originally tasked for you.");
        auto result3 = Engine::instance().waitForAgent(activeAgents.front());
        std::cout << "  -> Result: " << result3.value_or("Timeout") << std::endl;
        std::cout << "[TEST 8] Cleanup - terminating remaining agents..." << std::endl;
        for (const auto& id : activeAgents) {
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

}
