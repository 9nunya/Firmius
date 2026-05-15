#include "benchmarks/TuringSWEBenchPP.hpp"

namespace firmius::core {
namespace {
constexpr const char* kDatasetUrl =
    "https://datasets-server.huggingface.co/rows?dataset=TuringEnterprises/SWE-Bench-plus-plus&config=default&split=test&offset=0&limit=50";
constexpr const char* kDatasetCacheKey = "turingswebenchpp";
} // namespace

TuringSWEBenchPP::TuringSWEBenchPP(BenchmarkConfig config)
    : SWEBench(std::move(config), "turingswebenchpp", kDatasetUrl, kDatasetCacheKey) {}

} // namespace firmius::core
