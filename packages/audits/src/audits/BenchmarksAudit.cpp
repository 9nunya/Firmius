#include "audits/BenchmarksAudit.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "providers/ProviderRegistry.hpp"
#include "benchmarks/BenchmarkFactory.hpp"
#include "benchmarks/BenchmarkSession.hpp"
#include "utils/Logger.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace firmius::audits {

using namespace firmius::provider;
using namespace firmius::shared;
using namespace firmius::core;

namespace {

std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos)
    return "";
  size_t end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

void printUsage() {
  std::cout << "Usage: firmius_audit --bench <benchmark_name> [--provider "
               "<providerid>] [--model <modelid>]\n"
            << "\n"
            << "Options:\n"
            << "  --bench <name>     Benchmark to run (mbpp, swebench, "
               "agentbench)\n"
            << "  --provider <id>    Provider ID (default: nanogpt)\n"
            << "  --model <id>       Model ID (default: provider default)\n"
            << "\n"
            << "Examples:\n"
            << "  firmius_audit --bench mbpp --provider codex --model gpt-4o\n"
            << "  firmius_audit --bench swebench --provider antigravity --model "
               "gemini-3-flash\n"
            << std::endl;
}

} // namespace

std::string BenchmarksAudit::getId() const { return "benchmarks"; }

std::string BenchmarksAudit::getDescription() const {
  return "Run benchmark evaluations (MBPP, SWE-bench, AgentBench)";
}

shared::AuditResult BenchmarksAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();

  // Parse arguments
  std::string benchmarkName;
  std::string providerName = "nanogpt";
  std::string modelId;

  for (size_t i = 0; i < args.size(); ++i) {
    std::string arg = args[i];
    if (arg == "--bench" && i + 1 < args.size()) {
      benchmarkName = trim(args[++i]);
    } else if (arg == "--provider" && i + 1 < args.size()) {
      providerName = trim(args[++i]);
    } else if (arg == "--model" && i + 1 < args.size()) {
      modelId = trim(args[++i]);
    } else if (arg == "--help" || arg == "-h") {
      printUsage();
      result.exitCode = 0;
      result.passed = true;
      return result;
    }
  }

  if (benchmarkName.empty()) {
    Logger::instance().logError("--bench argument is required");
    printUsage();
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  const auto canonicalId = canonicalBenchmarkId(benchmarkName);
  if (!canonicalId.has_value()) {
    Logger::instance().logError("Unknown benchmark: " + benchmarkName);
    std::ostringstream supported;
    const auto ids = supportedBenchmarkIds();
    for (size_t i = 0; i < ids.size(); ++i) {
      if (i > 0) {
        supported << ", ";
      }
      supported << ids[i];
    }
    Logger::instance().logInfo("Supported benchmarks: " + supported.str());
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  // Load environment and initialize engine
  EnvLoader::load(".env.local");
  (void)firmius::core::Engine::instance();

  // Get provider
  auto provider = ProviderRegistry::instance().getProvider(providerName);
  if (!provider) {
    Logger::instance().logError("Unknown provider: " + providerName);
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  Logger::instance().logInfo("=== Benchmark Audit ===");
  Logger::instance().logInfo("Benchmark: " + benchmarkName);
  Logger::instance().logInfo("Provider:  " + providerName);
  if (!modelId.empty()) {
    Logger::instance().logInfo("Model:     " + modelId);
  }

  // Create benchmark configuration
  BenchmarkConfig config;
  config.providerId = providerName;
  if (!modelId.empty()) {
    config.modelId = modelId;
  }

  Logger::instance().logInfo("Initializing benchmark runner...");
  auto benchmark = makeBenchmark(*canonicalId, config);
  if (!benchmark) {
    Logger::instance().logError("Failed to initialize benchmark: " +
                                *canonicalId);
    result.exitCode = 1;
    result.passed = false;
    return result;
  }

  // List available tasks
  Logger::instance().logInfo("Loading benchmark tasks...");
  auto tasks = benchmark->listTasks();
  Logger::instance().logInfo("Found " + std::to_string(tasks.size()) +
                             " tasks");

  if (tasks.empty()) {
    Logger::instance().logWarning("No tasks available in benchmark");
    result.exitCode = 0;
    result.passed = true;
    result.output = "No tasks available";
    return result;
  }

  // Run a sample task (first task by default)
  std::string taskId = tasks[0];
  Logger::instance().logInfo("Preparing task: " + taskId);

  if (!benchmark->prepareTask(taskId)) {
    Logger::instance().logError("Failed to prepare task " + taskId);
    result.exitCode = 1;
    result.passed = false;
    result.output = "Failed to prepare task: " + taskId;
    return result;
  }

  Logger::instance().logInfo("Running task: " + taskId);
  auto taskResult = benchmark->runTask(taskId);

  // Report results
  Logger::instance().logInfo("=== Task Result ===");
  Logger::instance().logInfo("Task ID:       " + taskResult.taskId);
  Logger::instance().logInfo("Passed:        " +
                             std::string(taskResult.passed ? "YES" : "NO"));
  Logger::instance().logInfo("Total Tokens:  " +
                             std::to_string(taskResult.metrics.tokens.total));
  Logger::instance().logInfo("Prompt Tokens: " +
                             std::to_string(taskResult.metrics.tokens.prompt));
  Logger::instance().logInfo("Completion:    " +
                             std::to_string(taskResult.metrics.tokens.completion));

  if (!taskResult.output.empty()) {
    Logger::instance().logInfo("Output:\n" + taskResult.output);
  }

  result.exitCode = taskResult.passed ? 0 : 1;
  result.passed = taskResult.passed;
  result.output = taskResult.output;

  Logger::instance().logInfo("Benchmark audit completed");

  return result;
}

} // namespace firmius::audits
