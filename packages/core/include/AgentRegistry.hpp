#ifndef FIRMIUS_CORE_AGENT_REGISTRY_HPP
#define FIRMIUS_CORE_AGENT_REGISTRY_HPP

#include "IAgent.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace firmius::core {

using firmius::shared::IAgent;

/**
 * @brief Global registry for active agents.
 */
class AgentRegistry {
public:
    static AgentRegistry& instance() {
        static AgentRegistry i;
        return i;
    }

    void registerAgent(const std::string& id, std::shared_ptr<shared::IAgent> agent) {
        std::lock_guard<std::mutex> lock(mutex);
        agents[id] = agent;
    }

    void unregisterAgent(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex);
        agents.erase(id);
    }

    std::shared_ptr<shared::IAgent> getAgent(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = agents.find(id);
        return (it != agents.end()) ? it->second : nullptr;
    }

    std::vector<std::string> listAll() const {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> keys;
        for (const auto& [id, _] : agents) {
            keys.push_back(id);
        }
        return keys;
    }

private:
    AgentRegistry() = default;
    std::map<std::string, std::shared_ptr<IAgent>> agents;
    mutable std::mutex mutex;
};

}

#endif
