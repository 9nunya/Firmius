#include "benchmarks/BenchmarkFactory.hpp"
#include "benchmarks/AgentBench.hpp"
#include "benchmarks/MBPPBenchmark.hpp"
#include "benchmarks/SWEBench.hpp"
#include <algorithm>
#include <cctype>

namespace firmius::core {

namespace {

std::string normalizedLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

} // namespace

std::optional<std::string> canonicalBenchmarkId(const std::string &rawName) {
  const std::string name = normalizedLower(rawName);
  if (name == "mbpp" || name == "mostlybasicpythonproblems") {
    return "mbpp";
  }
  if (name == "swebench" || name == "swe" || name == "softwareengineering") {
    return "swebench";
  }
  if (name == "agentbench" || name == "agent" || name == "osinteraction") {
    return "agentbench";
  }
  return std::nullopt;
}

std::vector<std::string> supportedBenchmarkIds() {
  return {"mbpp", "swebench", "agentbench"};
}

std::unique_ptr<firmius::shared::IBenchmark>
makeBenchmark(const std::string &canonicalId, BenchmarkConfig config) {
  if (canonicalId == "mbpp") {
    return std::make_unique<MBPPBenchmark>(std::move(config));
  }
  if (canonicalId == "swebench") {
    return std::make_unique<SWEBench>(std::move(config));
  }
  if (canonicalId == "agentbench") {
    return std::make_unique<AgentBench>(std::move(config));
  }
  return nullptr;
}

} // namespace firmius::core
