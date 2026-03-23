#include "benchmarks/BenchmarkFactory.hpp"
#include <gtest/gtest.h>

using namespace firmius::core;

TEST(BenchmarkFactory, CanonicalBenchmarkAliasesAreSupported) {
  EXPECT_EQ(canonicalBenchmarkId("mbpp"), std::optional<std::string>("mbpp"));
  EXPECT_EQ(canonicalBenchmarkId("mostlybasicpythonproblems"),
            std::optional<std::string>("mbpp"));
  EXPECT_EQ(canonicalBenchmarkId("swe"), std::optional<std::string>("swebench"));
  EXPECT_EQ(canonicalBenchmarkId("softwareengineering"),
            std::optional<std::string>("swebench"));
  EXPECT_EQ(canonicalBenchmarkId("agent"), std::optional<std::string>("agentbench"));
  EXPECT_EQ(canonicalBenchmarkId("osinteraction"),
            std::optional<std::string>("agentbench"));
  EXPECT_EQ(canonicalBenchmarkId("unknown"), std::nullopt);
}

TEST(BenchmarkFactory, MakesAllSupportedBenchmarks) {
  BenchmarkConfig config;
  config.initializeHarness = false;
  config.personaName = "worker";
  config.cwd = "/work";

  for (const auto &id : supportedBenchmarkIds()) {
    auto instance = makeBenchmark(id, config);
    EXPECT_NE(instance, nullptr) << "Failed to instantiate benchmark id: " << id;
  }
}

