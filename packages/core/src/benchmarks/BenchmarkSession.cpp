#include "benchmarks/BenchmarkSession.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "EnvLoader.hpp"
#include "Panic.hpp"
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

void BenchmarkSession::ensureReady() {
  if (agent_) {
    return;
  }
  Panic::init();
  EnvLoader::load(".env.local");
  auto &harness = Harness::instance();
  harness.init();
  if (!config_.providerId.empty() || !config_.modelId.empty()) {
    auto cfg = harness.getConfig();
    if (!config_.providerId.empty()) {
      cfg.defaultProviderId = config_.providerId;
    }
    if (!config_.modelId.empty()) {
      cfg.defaultModelId = config_.modelId;
    }
    harness.updateConfig(cfg);
    harness.saveConfig();
  }
  if (config_.cwd.empty()) {
    config_.cwd = "/work";
  }
  if (config_.personaName.empty()) {
    config_.personaName = "builder";
  }
  threadId_ =
      harness.newThread(config_.hostOptions, config_.cwd, config_.personaName);
  if (threadId_.empty()) {
    throw std::runtime_error("Failed to create benchmark thread");
  }
  agentId_ = Engine::instance().createAgent(threadId_, config_.personaName,
                                            false, "", "lead", "");
  auto agentBase = AgentRegistry::instance().getAgent(agentId_);
  agent_ = std::dynamic_pointer_cast<Agent>(agentBase);
  if (!agent_) {
    throw std::runtime_error("Agent not available for benchmark session");
  }
  waitForAgentBoot(agent_);
}

void BenchmarkSession::waitForAgentBoot(const std::shared_ptr<Agent> &agent) {
  int attempts = 0;
  while (agent->isBooting() && attempts < 50) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    attempts++;
  }
  if (agent->isBooting()) {
    throw std::runtime_error("Agent did not finish booting");
  }
}

} // namespace firmius::core
