#ifndef FIRMIUS_CORE_BENCHMARK_FACTORY_HPP
#define FIRMIUS_CORE_BENCHMARK_FACTORY_HPP

#include "IBenchmark.hpp"
#include "benchmarks/BenchmarkSession.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core {

std::optional<std::string> canonicalBenchmarkId(const std::string &rawName);
std::vector<std::string> supportedBenchmarkIds();
std::unique_ptr<firmius::shared::IBenchmark>
makeBenchmark(const std::string &canonicalId, BenchmarkConfig config);

} // namespace firmius::core

#endif
