#ifndef FIRMIUS_CORE_MBPP_BENCHMARK_HPP
#define FIRMIUS_CORE_MBPP_BENCHMARK_HPP

#include "IBenchmark.hpp"
#include "benchmarks/BenchmarkSession.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

using firmius::shared::BenchmarkResult;

/**
 * @brief Benchmark runner for the MBPP (Mostly Basic Python Problems) dataset.
 */
class MBPPBenchmark : public shared::IBenchmark {
public:
    explicit MBPPBenchmark(BenchmarkConfig config);
    
    std::vector<std::string> listTasks() override;
    bool prepareTask(const std::string& taskId) override;
    BenchmarkResult runTask(const std::string& taskId) override;

private:
    /**
     * @brief Downloads and caches the MBPP dataset if not present.
     */
    void ensureDatasetLoaded();
    
    BenchmarkSession session;
    rapidjson::Document dataset;
    bool datasetLoaded = false;
};

}

#endif
