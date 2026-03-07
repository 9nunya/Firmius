#include "Engine.hpp"
#include "agents/Agent.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/NanoGPTProvider.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "persistence/ThreadManager.hpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>
#include <thread>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;

int main() {
    Panic::init();
    EnvLoader::load(".env.local");

    auto& engine = Engine::instance();
    
    // 0. Setup threads
    ThreadMetadata meta;
    meta.title = "Test Thread";
    meta.hostType = HostType::Local;
    meta.hostIdentifier = "local";
    meta.cwd = "/tmp";
    meta.leadPersona = "coder";
    std::string home = getenv("HOME") ? getenv("HOME") : "/root";
    ThreadManager tm(home + "/.firmius/threads");
    std::string thread1 = tm.createThread(meta);
    std::string thread2 = tm.createThread(meta);
    std::string thread3 = tm.createThread(meta);

    // 1. Multi-listener test
    std::atomic<int> eventCount{0};
    std::atomic<int> turnCount{0};
    engine.addEventListener([&](const AppEvent& ev) {
        eventCount++;
        if (std::holds_alternative<AgentTurnCompleted>(ev)) {
            turnCount++;
        }
    });

    // 2. Concurrent Summoning Test
    std::cout << "Summoning 3 agents..." << std::endl;
    std::string id1 = engine.summonAgent(thread1, "coder", "echo hello");
    std::string id2 = engine.summonAgent(thread2, "coder", "echo hello");
    std::string id3 = engine.summonAgent(thread3, "coder", "echo hello");

    // Give them a moment to register
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto active = engine.listActiveAgents();
    std::cout << "Active agents: " << active.size() << std::endl;
    if (active.size() != 3) {
        std::cerr << "FAILURE: Active agents count mismatch!" << std::endl;
        return 1;
    }

    // 3. Cancellation Test
    std::cout << "Cancelling agent 1..." << std::endl;
    engine.cancelAgent(id1);
    
    // 4. Persistence Test (ephemeral)
    std::cout << "Summoning ephemeral agent..." << std::endl;
    std::string ephemeralThread = tm.createThread(meta);
    std::string ephemeralId = engine.summonAgent(ephemeralThread, "coder", "echo ephemeral", false);
    
    // Let's wait for them
    engine.waitForAgent(id1).value();
    engine.waitForAgent(id2).value();
    engine.waitForAgent(id3).value();
    engine.waitForAgent(ephemeralId).value();

    // Verify ephemeral agent has no journal
    std::string journalPath = home + "/.firmius/threads/" + ephemeralThread + "/" + ephemeralId + ".jsonl";
    if (std::filesystem::exists(journalPath)) {
        std::cerr << "FAILURE: Journal file exists for ephemeral agent!" << std::endl;
        return 1;
    }
    std::cout << "Verified: No journal for ephemeral agent." << std::endl;

    std::cout << "Total events captured: " << eventCount.load() << std::endl;
    std::cout << "Total turn completions: " << turnCount.load() << std::endl;
    if (eventCount < 3) {
        std::cerr << "FAILURE: Event count too low!" << std::endl;
        return 1;
    }
    if (turnCount < 2) { // id2 and id3 should complete at least one turn
        std::cerr << "FAILURE: Turn completion count too low!" << std::endl;
        return 1;
    }

    std::cout << "SUCCESS: Fleet management verified." << std::endl;
    return 0;
}
