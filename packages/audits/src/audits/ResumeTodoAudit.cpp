#include "audits/ResumeTodoAudit.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "AgentRegistry.hpp"
#include "harness/Harness.hpp"
#include "agents/Agent.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <algorithm>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

std::string ResumeTodoAudit::getId() const { return "resume_todo"; }
std::string ResumeTodoAudit::getDescription() const { return "Reproduction for premature idle after resume with pending todo"; }

shared::AuditResult ResumeTodoAudit::run(const std::vector<std::string> &) {
    AuditResult result;
    result.auditId = getId();
    result.passed = false;

    std::cout << "Starting Resume Todo Audit..." << std::endl;
    Panic::init();
    EnvLoader::load(".env.local");
    auto &harness = Harness::instance();
    harness.init();

    std::string threadId = harness.newThread({}, "/tmp", "lead");
    std::cout << "[Audit] Thread ID: " << threadId << std::endl;
    harness.switchModel("antigravity", "gemini-3-flash", harness.focusedAgentId());
    if (threadId.empty()) {
        std::cerr << "[Audit] Failed to create thread." << std::endl;
        result.exitCode = 1;
        return result;
    }

    // Phase 1: Get the agent to create a todo and start working
    std::cout << "[Audit] Starting task..." << std::endl;
    harness.send("1. Use todo_write to add: 'task A' and 'task B'.\n"
                 "2. Execute 'task A' by calling process_execute to create file /tmp/task_a.txt.\n"
                 "3. Then stop and wait for my next instruction. Do NOT do task B yet.");

    std::string agentId;
    std::shared_ptr<Agent> agent;
    for (int i = 0; i < 50; ++i) {
        agentId = harness.focusedAgentId();
        if (!agentId.empty()) {
            agent = std::dynamic_pointer_cast<Agent>(AgentRegistry::instance().getAgent(agentId));
            if (agent) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!agent) {
        std::cerr << "[Audit] Could not find agent." << std::endl;
        result.exitCode = 1;
        return result;
    }

    // Wait for task A to be executed (detect tool call)
    std::cout << "[Audit] Waiting for tool execution..." << std::endl;
    bool toolSeen = false;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(60)) {
        if (agent->getContext().state.currentStatus == AgentStatus::ExecutingTool) {
            toolSeen = true;
            std::cout << "[Audit] Tool execution detected." << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!toolSeen) {
        std::cout << "[Audit] Timed out waiting for tool." << std::endl;
        result.exitCode = 1;
        return result;
    }

    // Phase 2: Cancel while it's busy
    std::cout << "[Audit] Cancelling..." << std::endl;
    harness.abort();

    while (agent->isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "[Audit] Agent stopped. Status: " << static_cast<int>(agent->getContext().state.currentStatus) << std::endl;

    // Phase 3: Resume
    std::cout << "[Audit] Resuming..." << std::endl;
    harness.resumeLast();

    std::cout << "[Audit] Monitoring for task B or premature IDLE..." << std::endl;
    bool taskBSeen = false;
    bool prematureIdle = false;
    start = std::chrono::steady_clock::now();
    
    AgentStatus lastStatus = AgentStatus::Idle;
    
    int transitionsToIdle = 0;
    bool hadProviderWaiting = false;
    
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(60)) {
        AgentStatus currentStatus = agent->getContext().state.currentStatus;
        if (currentStatus != lastStatus) {
            std::cout << "[Audit] Status changed: " << static_cast<int>(lastStatus) << " -> " << static_cast<int>(currentStatus) << std::endl;
            if (currentStatus == AgentStatus::Idle) {
                transitionsToIdle++;
            }
            if (currentStatus == AgentStatus::ProviderWaiting) {
                hadProviderWaiting = true;
            }
            lastStatus = currentStatus;
        }
        
        // If it starts another tool call after resuming, it might be task B or retrying A
        if (currentStatus == AgentStatus::ExecutingTool) {
            std::cout << "[Audit] Agent is executing tools again. Success." << std::endl;
            taskBSeen = true;
            break;
        }
        
        if (!agent->isRunning() && currentStatus == AgentStatus::Idle) {
            // It stopped. 
            // If it stopped after being in ProviderWaiting but without ever getting to ExecutingTool,
            // that matches the user's "Provider waiting... boom... Idle"
            if (hadProviderWaiting && !taskBSeen) {
                std::cout << "[Audit] Premature IDLE detected! (ProviderWaiting -> Idle with no tool calls)" << std::endl;
                prematureIdle = true;
                break;
            } else if (!taskBSeen) {
                // Just stopped without doing anything
                std::cout << "[Audit] Agent stopped without continuing." << std::endl;
                prematureIdle = true;
                break;
            } else {
                std::cout << "[Audit] Agent finished." << std::endl;
                break;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    result.passed = !prematureIdle || taskBSeen;
    if (prematureIdle && !taskBSeen) {
        std::cout << "[Audit] REPRODUCED: Premature idle bug detected." << std::endl;
        if (agent->getContext().state.fatalError) {
            std::cout << "[Audit] Fatal Error: " << *agent->getContext().state.fatalError << std::endl;
        }
    } else if (taskBSeen) {
        std::cout << "[Audit] SUCCESS: Agent continued normally." << std::endl;
    } else {
        std::cout << "[Audit] FAILED: Timed out or other error." << std::endl;
    }

    harness.shutdown();
    result.exitCode = result.passed ? 0 : 1;
    return result;
}

} // namespace firmius::audits
