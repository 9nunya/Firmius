#ifndef FIRMIUS_CORE_BENCHMARK_SESSION_HPP
#define FIRMIUS_CORE_BENCHMARK_SESSION_HPP

#include "agents/Agent.hpp"
#include "Enums.hpp"

namespace firmius::core {

struct BenchmarkConfig {
    shared::HostCreationOptions hostOptions;
    std::string cwd;
    std::string personaName;
    std::string providerId;
    std::string modelId;
};

class BenchmarkSession {
public:
    explicit BenchmarkSession(BenchmarkConfig config);

    Agent& getAgent();
    shared::IHost& getHost();

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
