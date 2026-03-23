#include "benchmarks/BenchmarkSession.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
#include "agents/PurposeLoader.hpp"
#include "harness/Harness.hpp"
#include <chrono>
#include <thread>

namespace firmius::core {

BenchmarkSession::BenchmarkSession(BenchmarkConfig config)
    : config_(std::move(config)) {}

Agent &BenchmarkSession::getAgent() {
  ensureReady();
  return *agent_;
}

shared::IHost &BenchmarkSession::getHost() {
  ensureReady();
  return *agent_->getHost();
}

const BenchmarkConfig &BenchmarkSession::config() const { return config_; }

void BenchmarkSession::emitLog(const std::string &message) const {
  if (config_.logCallback) {
    config_.logCallback(message);
  }
}

namespace {

std::string findResumeFailureDetails(const std::shared_ptr<Agent> &agent) {
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
      if (msgIt->role != shared::Role::Error) {
        continue;
      }
      for (const auto &part : msgIt->content) {
        if (auto err = std::get_if<shared::ErrorContent>(&part)) {
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

} // namespace

void BenchmarkSession::ensureReady() {
  if (agent_) {
    return;
  }
  auto &harness = Harness::instance();

  if (config_.initializeHarness) {
    Panic::init();
    EnvLoader::load(".env.local");
    harness.init();
  }
  if (config_.cwd.empty()) {
    config_.cwd = "/work";
  }
  if (config_.personaName.empty()) {
    config_.personaName = "lead";
  }

  if (!config_.existingThreadId.empty()) {
    threadId_ = config_.existingThreadId;
  } else {
    threadId_ =
        harness.newThread(config_.hostOptions, config_.cwd, config_.personaName);
    if (threadId_.empty()) {
      throw std::runtime_error("Failed to create benchmark thread");
    }
  }

  if (!config_.existingAgentId.empty()) {
    agentId_ = config_.existingAgentId;
  } else {
    std::string title = config_.personaName;
    try {
      title = PurposeLoader::load(config_.personaName).title;
    } catch (...) {
    }
    agentId_ = Engine::instance().createAgent(threadId_, config_.personaName,
                                              true, "", config_.personaName,
                                              title);
  }

  auto agentBase = AgentRegistry::instance().getAgent(agentId_);
  agent_ = std::dynamic_pointer_cast<Agent>(agentBase);
  if (!agent_) {
    throw std::runtime_error("Agent not available for benchmark session");
  }
  waitForAgentBoot(agent_);

  const bool hasModelOverride =
      !config_.providerId.empty() && !config_.modelId.empty();
  if (hasModelOverride) {
    if (!config_.modelVariant.empty()) {
      Engine::instance().switchAgentModel(agentId_, config_.providerId,
                                          config_.modelId,
                                          config_.modelVariant);
    } else {
      Engine::instance().switchAgentModel(agentId_, config_.providerId,
                                          config_.modelId);
    }
  }

  std::string modelLabel = agent_->getContext().config.providerId + "/" +
                           agent_->getContext().config.modelId;
  if (!agent_->getContext().config.modelVariant.empty()) {
    modelLabel += " (" + agent_->getContext().config.modelVariant + ")";
  }
  emitLog("Benchmark session ready with " + config_.personaName +
          " persona on " + modelLabel + ".");
}

void BenchmarkSession::waitForAgentBoot(const std::shared_ptr<Agent> &agent) {
  int attempts = 0;
  while (agent->isBooting() && attempts < 50) {
    if (const std::string resumeFailure = findResumeFailureDetails(agent);
        !resumeFailure.empty()) {
      throw std::runtime_error("Agent bootstrap failed: " + resumeFailure);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    attempts++;
  }
  if (const std::string resumeFailure = findResumeFailureDetails(agent);
      !resumeFailure.empty()) {
    throw std::runtime_error("Agent bootstrap failed: " + resumeFailure);
  }
  if (agent->isBooting()) {
    throw std::runtime_error("Agent did not finish booting");
  }
}

} // namespace firmius::core
