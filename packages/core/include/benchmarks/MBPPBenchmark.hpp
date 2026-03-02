#ifndef FIRMIUS_CORE_MBPP_BENCHMARK_HPP
#define FIRMIUS_CORE_MBPP_BENCHMARK_HPP

#include "IBenchmark.hpp"
#include "agents/Agent.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Benchmark runner for the MBPP (Mostly Basic Python Problems) dataset.
 */
class MBPPBenchmark : public shared::IBenchmark {
public:
    /**
     * @brief Constructs an MBPPBenchmark runner.
     * @param agent The agent instance to evaluate.
     * @param host The host environment to use for execution.
     */
    MBPPBenchmark(Agent& agent, shared::IHost& host);
    
    std::vector<std::string> listTasks() override;
    bool prepareTask(const std::string& taskId) override;
    BenchmarkResult runTask(const std::string& taskId) override;

private:
    /**
     * @brief Downloads and caches the MBPP dataset if not present.
     */
    void ensureDatasetLoaded();
    
    Agent& agent;
    shared::IHost& host;
    rapidjson::Document dataset;
    bool datasetLoaded = false;
};

}

#endif
