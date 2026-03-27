#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "Engine.hpp"
#include "Panic.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;

namespace {

class DelayedTextProvider : public IProvider {
public:
  explicit DelayedTextProvider(std::string providerId)
      : providerId_(std::move(providerId)) {}

  std::string getId() const override { return providerId_; }

  void stream(const AgentHistory &, const ProviderOptions &opts,
              std::function<void(const StreamEvent &)> onEvent) override {
    callCount_.fetch_add(1, std::memory_order_relaxed);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
      if (opts.abortSignal && opts.abortSignal->load()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (opts.abortSignal && opts.abortSignal->load()) {
      return;
    }
    onEvent(TextChunk{"fleet ok"});
    onEvent(StreamDone{StopReason::Stop});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = providerId_ + "-model";
    model.provider = providerId_;
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }

  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> *abortSignal = nullptr) override {
    if (abortSignal && abortSignal->load()) {
      return;
    }
    onEvent(TextChunk{"fleet ok"});
    onEvent(StreamDone{StopReason::Stop});
  }

  ProviderType getProviderType() const override {
    return ProviderType::APIKey;
  }

private:
  std::string providerId_;
  std::atomic<int> callCount_{0};
};

class ScopedFleetEnvironment {
public:
  ScopedFleetEnvironment() {
    originalHome_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    originalPromptsDir_ = std::getenv("FIRMIUS_PROMPTS_DIR")
                              ? std::getenv("FIRMIUS_PROMPTS_DIR")
                              : "";
    originalConfig_ = ConfigLoader::instance().getConfig();

    tempHome_ = std::filesystem::temp_directory_path() /
                ("firmius_fleet_home_" +
                 std::to_string(static_cast<long long>(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count())));
    promptsDir_ = tempHome_ / "prompts";
    std::filesystem::create_directories(tempHome_ / ".firmius" / "threads");
    std::filesystem::create_directories(promptsDir_);
    setenv("HOME", tempHome_.c_str(), 1);
    setenv("FIRMIUS_PROMPTS_DIR", promptsDir_.c_str(), 1);

    std::ofstream coderFile(promptsDir_ / "coder.md");
    coderFile << "---\nname: coder\ntitle: Coder\n---\nCoder identity\n";
    coderFile.close();
  }

  ~ScopedFleetEnvironment() {
    Engine::instance().shutdown();
    for (const auto &agentId : AgentRegistry::instance().listAll()) {
      AgentRegistry::instance().unregisterAgent(agentId);
    }
    ConfigLoader::instance().updateConfig(originalConfig_);
    std::error_code ec;
    std::filesystem::remove_all(tempHome_, ec);
    if (originalHome_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", originalHome_.c_str(), 1);
    }
    if (originalPromptsDir_.empty()) {
      unsetenv("FIRMIUS_PROMPTS_DIR");
    } else {
      setenv("FIRMIUS_PROMPTS_DIR", originalPromptsDir_.c_str(), 1);
    }
  }

  const std::filesystem::path &tempHome() const { return tempHome_; }

private:
  std::filesystem::path tempHome_;
  std::filesystem::path promptsDir_;
  std::string originalHome_;
  std::string originalPromptsDir_;
  UserConfig originalConfig_;
};

bool waitForActiveAgentCount(size_t expected,
                             std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (Engine::instance().listActiveAgents().size() == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return Engine::instance().listActiveAgents().size() == expected;
}

bool completedOutcome(const std::optional<AgentOutcome> &outcome) {
  return outcome.has_value() &&
         (outcome->kind == AgentOutcome::Kind::Response ||
          outcome->kind == AgentOutcome::Kind::NoSummary);
}

std::string describeOutcome(const std::optional<AgentOutcome> &outcome) {
  if (!outcome.has_value()) {
    return "<none>";
  }

  std::string kind;
  switch (outcome->kind) {
  case AgentOutcome::Kind::Response:
    kind = "Response";
    break;
  case AgentOutcome::Kind::NoSummary:
    kind = "NoSummary";
    break;
  case AgentOutcome::Kind::Cancelled:
    kind = "Cancelled";
    break;
  case AgentOutcome::Kind::Failed:
    kind = "Failed";
    break;
  }

  return kind + ": " + outcome->text;
}

} // namespace

int main() {
  Panic::init();
  ScopedFleetEnvironment env;

  auto provider = std::make_shared<DelayedTextProvider>("fleet-test-provider");
  ProviderRegistry::instance().registerProvider(provider);

  auto cfg = ConfigLoader::instance().getConfig();
  cfg.defaultProviderId = provider->getId();
  cfg.defaultModelId = provider->listModels().front().id;
  ConfigLoader::instance().updateConfig(cfg);

  auto &engine = Engine::instance();

  ThreadMetadata meta;
  meta.title = "Test Thread";
  meta.hostOptions.type = HostType::Local;
  meta.hostIdentifier = "local";
  meta.cwd = env.tempHome().string();
  meta.leadPersona = "coder";
  ThreadManager tm(ThreadManager::defaultBasePath());
  std::string thread1 = tm.createThread(meta);
  std::string thread2 = tm.createThread(meta);
  std::string thread3 = tm.createThread(meta);

  std::atomic<int> eventCount{0};
  std::atomic<int> turnCount{0};
  engine.addEventListener([&](const AppEvent &ev) {
    eventCount++;
    if (std::holds_alternative<AgentTurnCompleted>(ev)) {
      turnCount++;
    }
  });

  std::cout << "Summoning 3 agents..." << std::endl;
  std::string id1 = engine.summonAgent(thread1, "coder", "echo hello");
  std::string id2 = engine.summonAgent(thread2, "coder", "echo hello");
  std::string id3 = engine.summonAgent(thread3, "coder", "echo hello");

  if (!waitForActiveAgentCount(3, std::chrono::seconds(5))) {
    auto active = engine.listActiveAgents();
    std::cout << "Active agents: " << active.size() << std::endl;
    std::cerr << "FAILURE: Active agents count mismatch!" << std::endl;
    return 1;
  }

  std::cout << "Active agents: 3" << std::endl;
  std::cout << "Cancelling agent 1..." << std::endl;
  engine.cancelAgent(id1);

  std::cout << "Summoning ephemeral agent..." << std::endl;
  std::string ephemeralThread = tm.createThread(meta);
  std::string ephemeralId =
      engine.summonAgent(ephemeralThread, "coder", "echo ephemeral", false);

  auto outcome1 = engine.waitForAgentOutcome(id1);
  if (!outcome1.has_value() ||
      outcome1->kind != AgentOutcome::Kind::Cancelled) {
    std::cerr
        << "FAILURE: Agent 1 did not finish with a cancelled outcome!"
        << std::endl;
    return 1;
  }
  auto outcome2 = engine.waitForAgentOutcome(id2);
  if (!completedOutcome(outcome2)) {
    std::cerr
        << "FAILURE: Agent 2 did not finish with a successful typed outcome: "
        << describeOutcome(outcome2)
        << std::endl;
    return 1;
  }
  auto outcome3 = engine.waitForAgentOutcome(id3);
  if (!completedOutcome(outcome3)) {
    std::cerr
        << "FAILURE: Agent 3 did not finish with a successful typed outcome: "
        << describeOutcome(outcome3)
        << std::endl;
    return 1;
  }
  auto ephemeralOutcome = engine.waitForAgentOutcome(ephemeralId);
  if (!completedOutcome(ephemeralOutcome)) {
    std::cerr
        << "FAILURE: Ephemeral agent did not finish with a successful typed "
           "outcome: "
        << describeOutcome(ephemeralOutcome)
        << std::endl;
    return 1;
  }

  const std::string journalPath = ThreadManager::defaultBasePath() + "/" +
                                  ephemeralThread + "/" + ephemeralId +
                                  ".jsonl";
  if (std::filesystem::exists(journalPath)) {
    std::cerr << "FAILURE: Journal file exists for ephemeral agent!"
              << std::endl;
    return 1;
  }
  std::cout << "Verified: No journal for ephemeral agent." << std::endl;

  std::cout << "Total events captured: " << eventCount.load() << std::endl;
  std::cout << "Total turn completions: " << turnCount.load() << std::endl;
  if (eventCount < 3) {
    std::cerr << "FAILURE: Event count too low!" << std::endl;
    return 1;
  }
  if (turnCount < 2) {
    std::cerr << "FAILURE: Turn completion count too low!" << std::endl;
    return 1;
  }

  std::cout << "SUCCESS: Fleet management verified." << std::endl;
  return 0;
}
