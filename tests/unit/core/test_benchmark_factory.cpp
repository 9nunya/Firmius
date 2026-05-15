#include "benchmarks/BenchmarkFactory.hpp"
#include "benchmarks/SWEBenchTaskSpec.hpp"
#include <gtest/gtest.h>

using namespace firmius::core;

TEST(BenchmarkFactory, CanonicalBenchmarkAliasesAreSupported) {
  EXPECT_EQ(canonicalBenchmarkId("mbpp"), std::optional<std::string>("mbpp"));
  EXPECT_EQ(canonicalBenchmarkId("mostlybasicpythonproblems"),
            std::optional<std::string>("mbpp"));
  EXPECT_EQ(canonicalBenchmarkId("swe"), std::optional<std::string>("swebench"));
  EXPECT_EQ(canonicalBenchmarkId("softwareengineering"),
            std::optional<std::string>("swebench"));
  EXPECT_EQ(canonicalBenchmarkId("swebenchpp"),
            std::optional<std::string>("turingswebenchpp"));
  EXPECT_EQ(canonicalBenchmarkId("turingSWEBench++"),
            std::optional<std::string>("turingswebenchpp"));
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

TEST(BenchmarkFactory, ParsesStringEncodedTaskEnvironmentConfig) {
  rapidjson::Document doc;
  doc.Parse(R"({
    "instance_id":"task-1",
    "repo":"example/project",
    "base_commit":"abc123",
    "problem_statement":"Fix the failure",
    "test_patch":"diff --git a/x b/x",
    "FAIL_TO_PASS":"[\"tests/test_one.py::test_a\"]",
    "PASS_TO_PASS":"[\"tests/test_two.py::test_b\"]",
    "environment_config":"{\"env\":{\"FOO\":\"bar\"},\"install\":[\"uv sync\"],\"build\":[\"make build\"],\"test\":[\"pytest tests/test_one.py::test_a\"]}"
  })");

  const auto spec = parseSWEBenchTaskSpec(doc);
  ASSERT_EQ(spec.instanceId, "task-1");
  ASSERT_EQ(spec.repo, "example/project");
  ASSERT_EQ(spec.failToPass.size(), 1u);
  EXPECT_EQ(spec.failToPass[0], "tests/test_one.py::test_a");
  ASSERT_EQ(spec.passToPass.size(), 1u);
  EXPECT_EQ(spec.passToPass[0], "tests/test_two.py::test_b");
  ASSERT_EQ(spec.installCommands.size(), 1u);
  EXPECT_EQ(spec.installCommands[0], "uv sync");
  ASSERT_EQ(spec.buildCommands.size(), 1u);
  EXPECT_EQ(spec.buildCommands[0], "make build");
  ASSERT_EQ(spec.evalCommands.size(), 1u);
  EXPECT_EQ(spec.evalCommands[0], "pytest tests/test_one.py::test_a");
  ASSERT_EQ(spec.environment.size(), 1u);
  EXPECT_EQ(spec.environment.at("FOO"), "bar");
}

TEST(BenchmarkFactory, ParsesObjectTaskEnvironmentConfig) {
  rapidjson::Document doc;
  doc.Parse(R"({
    "instance_id":"task-2",
    "repo":"example/project",
    "base_commit":"def456",
    "problem_statement":"Fix another failure",
    "test_patch":"",
    "FAIL_TO_PASS":["tests/test_three.py::test_c"],
    "environment_config":{
      "environment":{"BAR":"baz"},
      "setup":"python -m pip install -r requirements.txt",
      "build":["cmake -S . -B build", "cmake --build build"],
      "evaluation":["ctest --test-dir build -R test_c"]
    }
  })");

  const auto spec = parseSWEBenchTaskSpec(doc);
  ASSERT_EQ(spec.failToPass.size(), 1u);
  EXPECT_EQ(spec.failToPass[0], "tests/test_three.py::test_c");
  ASSERT_EQ(spec.installCommands.size(), 1u);
  EXPECT_EQ(spec.installCommands[0], "python -m pip install -r requirements.txt");
  ASSERT_EQ(spec.buildCommands.size(), 2u);
  EXPECT_EQ(spec.buildCommands[0], "cmake -S . -B build");
  EXPECT_EQ(spec.buildCommands[1], "cmake --build build");
  ASSERT_EQ(spec.evalCommands.size(), 1u);
  EXPECT_EQ(spec.evalCommands[0], "ctest --test-dir build -R test_c");
  ASSERT_EQ(spec.environment.size(), 1u);
  EXPECT_EQ(spec.environment.at("BAR"), "baz");
}
