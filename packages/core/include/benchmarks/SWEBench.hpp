#ifndef FIRMIUS_CORE_SWE_BENCH_HPP
#define FIRMIUS_CORE_SWE_BENCH_HPP

#include "IBenchmark.hpp"
#include "benchmarks/BenchmarkSession.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Benchmark runner for SWE-bench (Software Engineering tasks).
 * Handles repository preparation, dependency installation, and baseline evaluation.
 */
class SWEBench : public shared::IBenchmark {
public:
    explicit SWEBench(BenchmarkConfig config);
    
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

    BenchmarkSession session;
    rapidjson::Document dataset;
    bool datasetLoaded = false;
};

}

#endif
