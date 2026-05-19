#ifndef FIRMIUS_CORE_BENCHMARKSESSION_HPP
#define FIRMIUS_CORE_BENCHMARKSESSION_HPP

#include "agents/Agent.hpp"
#include "Enums.hpp"
#include "Events.hpp"
#include <functional>

namespace firmius::core {

struct BenchmarkConfig {
    shared::HostCreationOptions hostOptions;
    std::string cwd;
    std::string personaName;
    std::string providerId;
    std::string modelId;
    std::string modelVariant;
    std::string existingThreadId;
    std::string existingAgentId;
    bool initializeHarness = true;
    std::function<void(const std::string&)> logCallback;
};

class BenchmarkSession {
public:
    explicit BenchmarkSession(BenchmarkConfig config);

    Agent& getAgent();
    shared::IHost& getHost();
    const BenchmarkConfig& config() const;
    const std::string& threadId() const { return threadId_; }
    const std::string& agentId() const { return agentId_; }
    void emitLog(const std::string& message) const;
    shared::AgentOutcome runAgentTask(
        const std::string &task,
        const std::vector<shared::ImageContent> &images = {});

private:
    void ensureReady();
    void waitForAgentBoot(const std::shared_ptr<Agent>& agent);

    BenchmarkConfig config_;
    std::string threadId_;
    std::string agentId_;
    std::shared_ptr<Agent> agent_;
};

}

#endif
