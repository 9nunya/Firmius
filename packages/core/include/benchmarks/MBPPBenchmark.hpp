#ifndef FIRMIUS_CORE_MBPP_BENCHMARK_HPP
#define FIRMIUS_CORE_MBPP_BENCHMARK_HPP

#include "benchmarks/shared::IBenchmark.hpp"
#include "agents/Agent.hpp"
#include <rapidjson/document.h>

namespace firmius::core {
using namespace firmius::shared;

class MBPPBenchmark : public shared::IBenchmark {
public:
    MBPPBenchmark(Agent& agent, shared::IHost& host);
    
    std::vector<std::string> listTasks() override;
    bool prepareTask(const std::string& taskId) override;
    BenchmarkResult runTask(const std::string& taskId) override;

private:
    void ensureDatasetLoaded();
    
    Agent& agent;
    shared::IHost& host;
    rapidjson::Document dataset;
    bool datasetLoaded = false;
};

}

#endif
