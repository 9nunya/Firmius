#ifndef FIRMIUS_CORE_SWEBENCH_HPP
#define FIRMIUS_CORE_SWEBENCH_HPP

#include "IBenchmark.hpp"
#include "benchmarks/BenchmarkSession.hpp"
#include "benchmarks/SWEBenchTaskSpec.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

using firmius::shared::BenchmarkResult;

/**
 * @brief Benchmark runner for SWE-bench (Software Engineering tasks).
 * Handles repository preparation, dependency installation, and baseline evaluation.
 */
class SWEBench : public shared::IBenchmark {
public:
    explicit SWEBench(BenchmarkConfig config);
    explicit SWEBench(BenchmarkConfig config, std::string benchmarkId,
                      std::string datasetUrl, std::string datasetCacheKey);
    
    std::vector<std::string> listTasks() override;
    bool prepareTask(const std::string& taskId) override;
    BenchmarkResult runTask(const std::string& taskId) override;

protected:
    const rapidjson::Value* findTaskRow(const std::string& taskId);
    SWEBenchTaskSpec requireTaskSpec(const std::string& taskId);
    std::string buildEvaluationCommand(const SWEBenchTaskSpec& spec) const;
    std::map<std::string, std::string> buildTestEnvironment(const SWEBenchTaskSpec& spec) const;

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
    std::string benchmarkId_;
    std::string datasetUrl_;
    std::string datasetCacheKey_;
    rapidjson::Document dataset;
    bool datasetLoaded = false;
};

}

#endif
