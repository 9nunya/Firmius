#ifndef FIRMIUS_SHARED_IBENCHMARK_HPP
#define FIRMIUS_SHARED_IBENCHMARK_HPP

#include "Metrics.hpp"
#include <string>
#include <vector>

/**
 * @brief Benchmark abstraction for agent performance evaluation.
 */
namespace firmius::shared {

/**
 * @brief Result summary for a single benchmark task execution.
 */
struct BenchmarkResult {
    std::string taskId;    ///< The unique task ID from the dataset.
    bool passed = false;   ///< True if the agent solved the task.
    AgentMetrics metrics;  ///< Telemetry captured during the run.
    std::string output;    ///< Raw stdout/stderr or evaluation logs.
};

/**
 * @brief Interface for a benchmark dataset runner.
 */
class IBenchmark {
public:
    virtual ~IBenchmark() = default;

    /**
     * @brief Lists all task IDs available in this benchmark.
     */
    virtual std::vector<std::string> listTasks() = 0;
    
    /**
     * @brief Prepares the execution environment for a specific task.
     * @param taskId Task to prepare.
     * @return True if preparation succeeded.
     */
    virtual bool prepareTask(const std::string& taskId) = 0;
    
    /**
     * @brief Executes the agent on the prepared task.
     * @param taskId Task to run.
     * @return Final benchmark result.
     */
    virtual BenchmarkResult runTask(const std::string& taskId) = 0;
};

}

#endif
