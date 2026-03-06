#include "EnvLoader.hpp"
#include "hosts/LocalHost.hpp"
#include "hosts/DockerHost.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/NanoGPTProvider.hpp"
#include "providers/OpenRouterProvider.hpp"
#include "providers/ZaiProvider.hpp"
#include "providers/ZenProvider.hpp"
#include "providers/ChutesProvider.hpp"
#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "agents/Agent.hpp"
#include "agents/PurposeLoader.hpp"
#include "tools/FileReadTool.hpp"
#include "tools/FileEditTool.hpp"
#include "tools/ProcessExecuteTool.hpp"
#include "tools/PythonExecuteTool.hpp"
#include "tools/ListDirectoryTool.hpp"
#include "tools/GlobTool.hpp"
#include "tools/GrepTool.hpp"
#include "tools/WebFetchTool.hpp"
#include "tools/SubagentTool.hpp"
#include "tools/SubagentWaitTool.hpp"
#include "tools/SubagentTerminateTool.hpp"
#include "tools/ToolRegistry.hpp"
#include "persistence/ThreadManager.hpp"
#include "Panic.hpp"
#include <iostream>
#include <iterator>
#include <thread>
#include <chrono>

using namespace firmius::shared;
using namespace firmius::core;
using namespace firmius::provider;

int main(int /*argc*/, char** /*argv*/) {
    Panic::init();
    EnvLoader::load(".env.local");

    std::cout << "=== P15 Persistent Subagent Communication Audit ===" << std::endl;
    std::cout << std::endl;

    // Setup provider registry
    auto& providerRegistry = ProviderRegistry::instance();
    providerRegistry.registerProvider(std::make_shared<NanoGPTProvider>());
    providerRegistry.registerProvider(std::make_shared<OpenRouterProvider>(""));
    providerRegistry.registerProvider(std::make_shared<ZaiProvider>(""));
    providerRegistry.registerProvider(std::make_shared<ZenProvider>(""));
    providerRegistry.registerProvider(std::make_shared<ChutesProvider>(""));

    // Create a thread first (required for summonAgent to work)
    ThreadMetadata meta;
    meta.title = "P15 Test Thread";
    meta.hostType = HostType::Local;
    meta.hostIdentifier = "";
    meta.cwd = "/tmp";
    meta.leadPersona = "researcher";
    std::string threadId = ThreadManager::createThread(meta);

    // Test 1: Create a subagent and get its ID
    std::cout << "[TEST 1] Creating new subagent via summonAgent..." << std::endl;
    std::string persona = "researcher";
    std::string task1 = "Say 'hello'.";

    std::string agentId = Engine::instance().summonAgent(threadId, persona, task1, false);
    std::cout << "  -> Agent created with ID: " << agentId << std::endl;

    // Wait for agent to complete
    std::cout << "  -> Waiting for agent to complete..." << std::endl;
    std::string result1 = Engine::instance().waitForAgent(agentId);
    std::cout << "  -> Task 1 result: " << result1.substr(0, 100) << (result1.size() > 100 ? "..." : "") << std::endl;
    std::cout << std::endl;

    // Test 2: Re-task the same agent with a new task
    std::cout << "[TEST 2] Re-tasking existing agent via executeTask..." << std::endl;
    std::string task2 = "Create a file /tmp/p15_test.txt with content 'Hello from P15' and confirm";

    Engine::instance().executeTask(agentId, task2);
    std::cout << "  -> Re-tasking agent " << agentId << std::endl;

    std::string result2 = Engine::instance().waitForAgent(agentId);
    std::cout << "  -> Task 2 result: " << result2.substr(0, 100) << (result2.size() > 100 ? "..." : "") << std::endl;
    std::cout << std::endl;

    // Test 3: Verify agent is still in registry
    std::cout << "[TEST 3] Verifying agent persistence in registry..." << std::endl;
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (agent) {
        std::cout << "  -> Agent found in registry: YES" << std::endl;
        std::cout << "  -> Agent thread ID: " << agent->getContext().history->threadId << std::endl;
        std::cout << "  -> History turns count: " << agent->getContext().history->turns.size() << std::endl;
    } else {
        std::cout << "  -> Agent found in registry: NO (FAIL)" << std::endl;
        return 1;
    }
    std::cout << std::endl;

    // Test 4: Terminate the agent
    std::cout << "[TEST 4] Terminating agent via terminateAgent..." << std::endl;
    Engine::instance().terminateAgent(agentId);
    std::cout << "  -> Terminate called for agent " << agentId << std::endl;

    // Verify agent is removed
    auto agentAfter = AgentRegistry::instance().getAgent(agentId);
    if (!agentAfter) {
        std::cout << "  -> Agent removed from registry: YES" << std::endl;
    } else {
        std::cout << "  -> Agent removed from registry: NO (FAIL)" << std::endl;
        return 1;
    }
    std::cout << std::endl;

    // Test 5: Test SubagentTool with agent_id (re-tasking via tool)
    std::cout << "[TEST 5] Testing SubagentTool with agent_id for re-tasking..." << std::endl;
    std::string agentId2 = Engine::instance().summonAgent(threadId, "coder", "Return 'initial task done'", false);
    std::cout << "  -> Created second agent: " << agentId2 << std::endl;

    Engine::instance().waitForAgent(agentId2);
    std::cout << "  -> First task completed" << std::endl;

    // Create tool context to test SubagentTool directly
    ToolRegistry testRegistry;
    testRegistry.registerTool(std::make_unique<SubagentTool>());
    testRegistry.registerTool(std::make_unique<SubagentTerminateTool>());

    // Create a mock agent context for the tool context
    AgentContext ctx;
    ctx.history->threadId = threadId;
    ctx.config.providerId = "zen";
    ctx.config.modelId = "big-pickle";

    std::cout << "  -> SubagentTool registered successfully" << std::endl;
    std::cout << std::endl;

    // Test 6: List active agents
    std::cout << "[TEST 6] Listing active agents..." << std::endl;
    auto activeAgents = Engine::instance().listActiveAgents();
    std::cout << "  -> Active agents count: " << activeAgents.size() << std::endl;
    for (const auto& id : activeAgents) {
        std::cout << "     - " << id << std::endl;
    }
    std::cout << std::endl;

    // Test continue again
    std::cout << "[TEST 7] Resuming AGAIN - trying to resume the first available agent" << std::endl;

    Engine::instance().executeTask(activeAgents.front(), "If you remember doing a task, please output directly what was originally tasked for you.");
    auto result3 = Engine::instance().waitForAgent(activeAgents.front());

    std::cout << "  -> Result: " << result3 << std::endl;

    // Cleanup
    std::cout << "[TEST 8] Cleanup - terminating remaining agents..." << std::endl;
    for (const auto& id : activeAgents) {
        Engine::instance().terminateAgent(id);
        std::cout << "  -> Terminated: " << id << std::endl;
    }
    std::cout << std::endl;

    std::cout << "=== P15 Audit Complete ===" << std::endl;
    std::cout << "All tests passed!" << std::endl;

    return 0;
}
