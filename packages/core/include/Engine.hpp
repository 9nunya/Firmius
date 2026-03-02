#ifndef FIRMIUS_CORE_ENGINE_HPP
#define FIRMIUS_CORE_ENGINE_HPP

#include "IAgent.hpp"
#include "IHost.hpp"
#include "IProvider.hpp"
#include "Events.hpp"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <future>
#include <map>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief The fleet commander. Manages agent lifecycles and event distribution.
 */
class Engine {
public:
    static Engine& instance() {
        static Engine i;
        return i;
    }

    /**
     * @brief Summons a new agent in its own thread.
     */
    std::string summonAgent(const std::string& threadId, const std::string& personaName, const std::string& task = "", bool persistHistory = true);

    /**
     * @brief Waits for an agent to complete and returns its summary.
     */
    std::string waitForAgent(const std::string& agentId);

    /**
     * @brief Adds a listener for all engine events.
     */
    void addEventListener(std::function<void(const EngineEvent&)> listener);

    /**
     * @brief Cancels a running agent.
     */
    void cancelAgent(const std::string& agentId);

    /**
     * @brief Lists all currently active agent IDs.
     */
    std::vector<std::string> listActiveAgents() const;

    /**
     * @brief Internal: broadcasts an event to all listeners.
     */
    void broadcast(const EngineEvent& event);

private:
    Engine();
    void initProviders();
    void reap();

    std::vector<std::function<void(const EngineEvent&)>> listeners;
    std::mutex listenerMutex;
    std::vector<std::jthread> fleet;
    
    std::map<std::string, std::shared_future<std::string>> agentFutures;
    std::mutex futuresMutex;

    size_t maxConcurrentAgents = 10;
};

}

#endif
