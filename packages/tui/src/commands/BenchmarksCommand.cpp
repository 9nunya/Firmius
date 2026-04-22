#include "commands/BenchmarksCommand.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "NotificationManager.hpp"
#include "benchmarks/BenchmarkFactory.hpp"
#include "benchmarks/BenchmarkSession.hpp"
#include "harness/Harness.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <thread>

namespace firmius::tui {

namespace {

std::string supportedBenchmarksSummary() {
  const auto ids = firmius::core::supportedBenchmarkIds();
  std::string joined;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i > 0) {
      joined += ", ";
    }
    joined += ids[i];
  }
  return joined;
}

bool dockerSandboxImageReady() {
  // Silence command output to avoid polluting the TUI render buffer.
  return std::system("docker image inspect firmius-sandbox:latest > /dev/null 2>&1") ==
         0;
}

std::string findResumeFailureDetails(const std::shared_ptr<firmius::core::Agent> &agent) {
  if (!agent) {
    return "Agent unavailable during benchmark bootstrap.";
  }
  const auto history = agent->getContext().history;
  if (!history) {
    return "";
  }

  for (auto turnIt = history->turns.rbegin(); turnIt != history->turns.rend();
       ++turnIt) {
    for (auto msgIt = turnIt->messages.rbegin(); msgIt != turnIt->messages.rend();
         ++msgIt) {
      if (msgIt->role != firmius::shared::Role::Error) {
        continue;
      }
      for (const auto &part : msgIt->content) {
        if (auto err = std::get_if<firmius::shared::ErrorContent>(&part)) {
          if (err->description == "Resume agent failed." ||
              err->errorName == "Engine Error") {
            if (!err->details.empty()) {
              return err->details;
            }
            return err->description.empty() ? "Resume agent failed."
                                            : err->description;
          }
        }
      }
    }
  }
  return "";
}

bool waitForAgentReady(const std::string &agentId, std::string &failureReason) {
  constexpr auto kTimeout = std::chrono::seconds(12);
  constexpr auto kPoll = std::chrono::milliseconds(100);
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;

  while (std::chrono::steady_clock::now() < deadline) {
    auto agentBase = firmius::core::AgentRegistry::instance().getAgent(agentId);
    auto agent = std::dynamic_pointer_cast<firmius::core::Agent>(agentBase);
    if (!agent) {
      failureReason = "Worker agent disappeared before boot completed.";
      return false;
    }

    if (const std::string resumeFailure = findResumeFailureDetails(agent);
        !resumeFailure.empty()) {
      failureReason = resumeFailure;
      return false;
    }

    if (!agent->isBooting()) {
      return true;
    }

    std::this_thread::sleep_for(kPoll);
  }

  auto agentBase = firmius::core::AgentRegistry::instance().getAgent(agentId);
  auto agent = std::dynamic_pointer_cast<firmius::core::Agent>(agentBase);
  if (const std::string resumeFailure = findResumeFailureDetails(agent);
      !resumeFailure.empty()) {
    failureReason = resumeFailure;
    return false;
  }

  failureReason = "Worker agent bootstrap timed out.";
  return false;
}

} // namespace

void BenchmarksCommand::execute(CommandCtx &ctx,
                                const std::vector<ParsedArg> &args) {
  if (!ctx.state) {
    return;
  }

  if (ctx.state->hasActiveThread()) {
    NotificationManager::instance().notifyInfo(
        "Benchmarks", "Closing current thread and starting benchmark run.");
  }

  const std::string benchmarkArg =
      args.empty() ? "" : firmius::shared::StringUtil::trim(args[0].raw_value);
  const std::string requestedTaskId =
      args.size() < 2 ? ""
                      : firmius::shared::StringUtil::trim(args[1].raw_value);

  if (benchmarkArg.empty()) {
    NotificationManager::instance().notifyInfo(
        "Benchmarks",
        "Usage: /benchmarks <benchmark> [task_id]. Supported: " +
            supportedBenchmarksSummary());
    return;
  }

  const auto canonical = firmius::core::canonicalBenchmarkId(benchmarkArg);
  if (!canonical.has_value()) {
    NotificationManager::instance().notifyError(
        "Benchmarks",
        "Unknown benchmark '" + benchmarkArg + "'. Supported: " +
            supportedBenchmarksSummary(),
        false);
    return;
  }

  auto &harness = firmius::core::Harness::instance();
  const auto config = harness.getConfig();

  if (!dockerSandboxImageReady()) {
    NotificationManager::instance().notifyError(
        "Benchmarks",
        "Docker is unavailable or image 'firmius-sandbox:latest' is missing. "
        "Start Docker and build/pull the sandbox image before running /benchmarks.",
        false);
    return;
  }

  firmius::shared::HostCreationOptions hostOptions;
  hostOptions.type = firmius::shared::HostType::Docker;
  hostOptions.connectToExisting = false;
  hostOptions.deleteOnExit = true;
  if (*canonical == "swebench") {
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "/root";
    const std::string cacheDir = home + "/.firmius/cache/swebench/repos";
    hostOptions.volumeMounts.push_back(cacheDir + ":/host_cache");
  }

  const std::string threadId = harness.newThread(hostOptions, "/work", "forge");
  if (threadId.empty()) {
    NotificationManager::instance().notifyError(
        "Benchmarks", "Failed to create benchmark thread.", false);
    return;
  }
  harness.setCurrentThreadPermissionMode(ctx.state->currentThreadPermissionMode());

  std::string agentId;
  try {
    agentId = firmius::core::Engine::instance().createAgent(
        threadId, "forge", true, "", "forge", "Benchmark Forge");
  } catch (const std::exception &ex) {
    NotificationManager::instance().notifyError(
        "Benchmarks", "Failed to create worker agent: " + std::string(ex.what()),
        false);
    return;
  }

  if (agentId.empty()) {
    NotificationManager::instance().notifyError(
        "Benchmarks", "Failed to create worker agent.", false);
    return;
  }

  if (!config.defaultProviderId.empty() && !config.defaultModelId.empty()) {
    try {
      if (!config.defaultModelVariant.empty()) {
        firmius::core::Engine::instance().switchAgentModel(
            agentId, config.defaultProviderId, config.defaultModelId,
            config.defaultModelVariant);
      } else {
        firmius::core::Engine::instance().switchAgentModel(
            agentId, config.defaultProviderId, config.defaultModelId);
      }
    } catch (...) {
    }
  }

  std::string bootFailureReason;
  if (!waitForAgentReady(agentId, bootFailureReason)) {
    harness.appendSystemMessage(
        agentId, "Benchmark bootstrap aborted: " + bootFailureReason);
    NotificationManager::instance().notifyError(
        "Benchmarks", "Failed to start benchmark worker: " + bootFailureReason,
        false);
    return;
  }

  harness.setFocusedAgent(agentId);
  harness.markThreadAsBenchmark(threadId, *canonical, requestedTaskId);
  harness.appendSystemMessage(
      agentId, "Benchmark run started: " + *canonical + ".");
  harness.appendSystemMessage(
      agentId,
      "Benchmark host initialized in Docker; setup and clone steps will stream here.");
  harness.appendSystemMessage(
      agentId,
      "Worker model: " + config.defaultProviderId + "/" + config.defaultModelId +
          (config.defaultModelVariant.empty()
               ? ""
               : (" (" + config.defaultModelVariant + ")")) +
          ".");

  std::thread([canonicalBenchmark = *canonical, requestedTaskId, config, threadId,
               agentId]() {
    auto &harnessRef = firmius::core::Harness::instance();
    auto log = [agentId](const std::string &message) {
      firmius::core::Harness::instance().appendSystemMessage(agentId, message);
    };

    try {
      firmius::core::BenchmarkConfig benchmarkConfig;
      benchmarkConfig.hostOptions.type = firmius::shared::HostType::Docker;
      benchmarkConfig.cwd = "/work";
      benchmarkConfig.personaName = "forge";
      benchmarkConfig.providerId = config.defaultProviderId;
      benchmarkConfig.modelId = config.defaultModelId;
      benchmarkConfig.modelVariant = config.defaultModelVariant;
      benchmarkConfig.existingThreadId = threadId;
      benchmarkConfig.existingAgentId = agentId;
      benchmarkConfig.initializeHarness = false;
      benchmarkConfig.logCallback = log;

      auto benchmark = firmius::core::makeBenchmark(canonicalBenchmark, benchmarkConfig);
      if (!benchmark) {
        log("Benchmark failed: unsupported benchmark '" + canonicalBenchmark +
            "'.");
        return;
      }

      log("Loading benchmark tasks for '" + canonicalBenchmark + "'.");
      auto tasks = benchmark->listTasks();
      if (tasks.empty()) {
        log("No tasks are available for this benchmark.");
        return;
      }

      std::string taskId = requestedTaskId;
      static thread_local std::mt19937 rng(std::random_device{}());
      auto pickRandomTask = [&]() -> std::string {
        std::uniform_int_distribution<size_t> dist(0, tasks.size() - 1);
        return tasks[dist(rng)];
      };
      if (taskId.empty()) {
        taskId = pickRandomTask();
        log("No task_id provided. Randomly selected task: " + taskId + ".");
      }
      if (!requestedTaskId.empty()) {
        const auto found = std::find(tasks.begin(), tasks.end(), requestedTaskId);
        if (found == tasks.end()) {
          taskId = pickRandomTask();
          log("Requested task '" + requestedTaskId +
              "' not found. Falling back to random task: " + taskId + ".");
        }
      }
      harnessRef.markThreadAsBenchmark(threadId, canonicalBenchmark, taskId);

      log("Preparing benchmark task: " + taskId + ".");
      if (!benchmark->prepareTask(taskId)) {
        log("Benchmark task preparation failed for " + taskId + ".");
        return;
      }

      log("Running benchmark task: " + taskId + ".");
      auto result = benchmark->runTask(taskId);
      log(std::string("Benchmark result: ") + (result.passed ? "PASS" : "FAIL") +
          " for task " + result.taskId + ".");
      if (!result.output.empty()) {
        log("Benchmark output:\n" + result.output);
      }
    } catch (const std::exception &ex) {
      log("Benchmark failed with runtime error: " + std::string(ex.what()));
    } catch (...) {
      log("Benchmark failed with unknown runtime error.");
    }
  }).detach();
}

} // namespace firmius::tui
