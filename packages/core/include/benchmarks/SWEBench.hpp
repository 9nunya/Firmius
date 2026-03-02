#ifndef FIRMIUS_CORE_SWE_BENCH_HPP
#define FIRMIUS_CORE_SWE_BENCH_HPP

#include "IBenchmark.hpp"
#include "agents/Agent.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Benchmark runner for SWE-bench (Software Engineering tasks).
 * Handles repository preparation, dependency installation, and baseline evaluation.
 */
class SWEBench : public shared::IBenchmark {
public:
    /**
     * @brief Constructs an SWEBench runner.
     * @param agent The agent instance to evaluate.
     * @param host The host environment to use for execution.
     */
    SWEBench(Agent& agent, shared::IHost& host);
    
    std::vector<std::string> listTasks() override;
    bool prepareTask(const std::string& taskId) override;
    BenchmarkResult runTask(const std::string& taskId) override;

private:
    /**
     * @brief Downloads and caches the SWE-bench dataset if not present.
     */
    void ensureDatasetLoaded();

    /**
     * @brief Parses test execution output using regex to extract pass/fail counts.
     */
    bool parseTestResults(const std::string& output, int& passed, int& failed, int& errors);

    Agent& agent;
    shared::IHost& host;
    rapidjson::Document dataset;
    bool datasetLoaded = false;
};

}

#endif
