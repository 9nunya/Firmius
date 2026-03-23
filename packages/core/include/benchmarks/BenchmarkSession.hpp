#ifndef FIRMIUS_CORE_BENCHMARK_SESSION_HPP
#define FIRMIUS_CORE_BENCHMARK_SESSION_HPP

#include "agents/Agent.hpp"
#include "Enums.hpp"
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
    void emitLog(const std::string& message) const;

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
