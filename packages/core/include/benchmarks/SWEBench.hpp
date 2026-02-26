#ifndef FIRMIUS_CORE_SWE_BENCH_HPP
#define FIRMIUS_CORE_SWE_BENCH_HPP

#include "benchmarks/shared::IBenchmark.hpp"
#include "agents/Agent.hpp"
#include <rapidjson/document.h>

namespace firmius::core {
using namespace firmius::shared;

class SWEBench : public shared::IBenchmark {
public:
    SWEBench(Agent& agent, shared::IHost& host);
    
    std::vector<std::string> listTasks() override;
    bool prepareTask(const std::string& taskId) override;
    BenchmarkResult runTask(const std::string& taskId) override;

private:
    void ensureDatasetLoaded();
    bool parseTestResults(const std::string& output, int& passed, int& failed, int& errors);

    Agent& agent;
    shared::IHost& host;
    rapidjson::Document dataset;
    bool datasetLoaded = false;
};

}

#endif
