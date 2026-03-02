#include "EnvLoader.hpp"
#include "persistence/ThreadManager.hpp"
#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "Panic.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <map>
#include <sstream>
#include <filesystem>

using namespace firmius::shared;
using namespace firmius::core;

namespace {
std::string currentThreadId;
std::string focusedAgentId;
std::mutex coutMutex;

void printPrompt() {
    std::cout << "\n\033[1;32mfirmius [" << currentThreadId.substr(0, 8) << "] > \033[0m" << std::flush;
}

void onEngineEvent(const EngineEvent& ev) {
    std::lock_guard<std::mutex> lock(coutMutex);
    
    if (auto* s = std::get_if<AgentSpawned>(&ev)) {
        std::cout << "\n\033[1;35m[Agent Spawned: " << s->personaName << " (" << s->agentId.substr(0, 8) << ")]\033[0m\n";
    } else if (auto* t = std::get_if<AgentThinking>(&ev)) {
        if (t->agentId == focusedAgentId) std::cout << "\033[3;37m" << t->delta << "\033[0m" << std::flush;
    } else if (auto* tx = std::get_if<AgentText>(&ev)) {
        if (tx->agentId == focusedAgentId) std::cout << tx->delta << std::flush;
    } else if (auto* tc = std::get_if<AgentToolCall>(&ev)) {
        if (tc->agentId == focusedAgentId) std::cout << "\n\033[1;33m[Tool: " << tc->toolName << "(" << tc->toolArgs << ")]\033[0m\n";
    } else if (auto* c = std::get_if<AgentCompleted>(&ev)) {
        std::cout << "\n\033[1;32m[Agent Completed: " << c->agentId.substr(0, 8) << "]\033[0m\n";
    } else if (auto* e = std::get_if<AgentError>(&ev)) {
        std::cout << "\n\033[1;31m[Agent Error (" << e->agentId.substr(0, 8) << "): " << e->message << "]\033[0m\n";
    }
}
}

int main() {
    Panic::init();
    EnvLoader::load(".env.local");
    Engine::instance().setEventListener(onEngineEvent);

    std::cout << "\033[1;34m=== FIRMIUS COMMAND HUB ===\033[0m\n";
    
    ThreadMetadata meta;
    meta.title = "Interactive Session";
    meta.hostType = HostType::Local;
    meta.hostIdentifier = "local";
    meta.cwd = std::filesystem::current_path().string();
    meta.leadPersona = "general";
    
    currentThreadId = ThreadManager::createThread(meta);

    std::string line;
    while (true) {
        printPrompt();
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (line[0] == '/') {
            std::stringstream ss(line);
            std::string cmd;
            ss >> cmd;

            if (cmd == "/help") {
                std::cout << "Commands: /new, /switch [id], /threads, /purpose [persona], /exit\n";
            } else if (cmd == "/exit") {
                break;
            } else if (cmd == "/threads") {
                for (const auto& t : ThreadManager::listThreads()) std::cout << " - " << t << "\n";
            } else if (cmd == "/new") {
                currentThreadId = ThreadManager::createThread(meta);
            } else if (cmd == "/switch") {
                ss >> focusedAgentId;
                std::cout << "Focus: " << focusedAgentId << "\n";
            } else if (cmd == "/purpose") {
                ss >> meta.leadPersona;
                std::cout << "Lead persona set to: " << meta.leadPersona << "\n";
            }
        } else {
            focusedAgentId = Engine::instance().summonAgent(currentThreadId, meta.leadPersona, line);
        }
    }

    std::cout << "Shutting down...\n";
    return 0;
}
