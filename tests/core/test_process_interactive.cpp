#include "Panic.hpp"
#include "agents/Agent.hpp"
#include "environment/Environment.hpp"
#include "hosts/LocalHost.hpp"
#include "providers/NanoGPTProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include "tools/ProcessTool.hpp"
#include "tools/ToolRegistry.hpp"
#include "../unit/mocks/MockPermissions.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <algorithm>

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::provider;

namespace {

template <typename Fn>
bool waitForCondition(Fn &&fn,
                      std::chrono::milliseconds timeout =
                          std::chrono::milliseconds(2000),
                      std::chrono::milliseconds step =
                          std::chrono::milliseconds(20)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) {
      return true;
    }
    std::this_thread::sleep_for(step);
  }
  return fn();
}

struct CancelProbeInput {
  int loops = 8;
  int sleepMs = 200;
};

struct CancelProbeState {
  std::atomic<int> started{0};
  std::atomic<int> cancelledObserved{0};
  std::atomic<int> sideEffectsAfterNextRun{0};
  std::atomic<bool> nextRunStarted{false};
};

class CancelProbeTool : public TypedTool<CancelProbeInput> {
public:
  explicit CancelProbeTool(std::shared_ptr<CancelProbeState> state)
      : state_(std::move(state)) {}

  ToolMetadata getMetadata() const override {
    return {"cancel_probe", "Long-running cancellation probe tool",
            ToolScope::Process};
  }

  std::shared_ptr<JSONSchema> getSchema() const override {
    return zObject({{"loops", zNumber()}, {"sleep_ms", zNumber()}});
  }

  CancelProbeInput transform(const rapidjson::Value &json) override {
    CancelProbeInput input;
    if (json.HasMember("loops") && json["loops"].IsInt()) {
      input.loops = json["loops"].GetInt();
    }
    if (json.HasMember("sleep_ms") && json["sleep_ms"].IsInt()) {
      input.sleepMs = json["sleep_ms"].GetInt();
    }
    return input;
  }

  ToolResult execute(const CancelProbeInput &input, ToolContext &ctx) override {
    state_->started.fetch_add(1);
    const int loops = std::max(1, input.loops);
    const auto sleepDuration =
        std::chrono::milliseconds(std::max(1, input.sleepMs));

    for (int i = 0; i < loops; ++i) {
      std::this_thread::sleep_for(sleepDuration);
      if (ctx.cancelRequested()) {
        state_->cancelledObserved.fetch_add(1);
        return ToolResult::fail("Interrupted");
      }
      if (state_->nextRunStarted.load()) {
        state_->sideEffectsAfterNextRun.fetch_add(1);
      }
    }

    return ToolResult::ok("{}");
  }

private:
  std::shared_ptr<CancelProbeState> state_;
};

class CancelProbeProvider : public IProvider {
public:
  std::string getId() const override { return "cancel-probe-provider"; }

  void stream(const AgentHistory &,
              const firmius::provider::ProviderOptions &,
              std::function<void(const StreamEvent &)> onEvent) override {
    const int call = callCount_.fetch_add(1);
    if (call == 0) {
      onEvent(ToolCallChunk{"cancel-probe-a", 0, "cancel_probe",
                            R"({"loops":8,"sleep_ms":250})"});
      onEvent(ToolCallChunk{"cancel-probe-b", 1, "cancel_probe",
                            R"({"loops":8,"sleep_ms":250})"});
      onEvent(StreamDone{StopReason::ToolUse});
      return;
    }
    onEvent(TextChunk{"follow-up done"});
    onEvent(StreamDone{StopReason::Stop});
  }

  std::vector<ModelInfo> listModels() override {
    ModelInfo model;
    model.id = "cancel-probe-model";
    model.provider = getId();
    model.contextWindow = 4096;
    return {model};
  }

  ModelInfo getModelInfo(const std::string &) override {
    return listModels().front();
  }

  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> * = nullptr) override {
    onEvent(TextChunk{"summary"});
    onEvent(StreamDone{StopReason::Stop});
  }

  firmius::provider::ProviderType getProviderType() const override {
    return firmius::provider::ProviderType::APIKey;
  }

  int callCount() const { return callCount_.load(); }

private:
  std::atomic<int> callCount_{0};
};

} // namespace

class ProcessInteractiveTest : public ::testing::Test {
protected:
  void SetUp() override {
    Panic::init();

    // Register a dummy provider for Agent construction
    auto &reg = ProviderRegistry::instance();
    if (!reg.getProvider("nanogpt")) {
      reg.registerProvider(std::make_shared<NanoGPTProvider>());
    }

    // Setup AgentContext
    AgentContext ctx;
    ctx.config.providerId = "nanogpt";
    ctx.config.modelId = "test-model";
    ctx.environment.type = HostType::Local;
    ctx.environment.cwd = "/tmp";
    ctx.permissions.allowedScopes = {ToolScope::Process};
    ctx.permissions.allowedPaths = {"/tmp"};

    // Registry and tools
    registry.registerTool(std::make_unique<ProcessTool>());

    auto localHost = std::make_shared<LocalHost>();
    hostRaw = localHost.get();
    auto environment = std::make_shared<Environment>(
        localHost, ctx.environment.cwd,
        [](const StreamEvent & /*ev*/) {});
    auto permissions = std::make_shared<firmius::test::MockPermissions>();
    permissions->cwd_ = ctx.environment.cwd;
    permissions->allowedPaths_ = ctx.permissions.allowedPaths;
    agent = std::make_unique<Agent>(ctx, environment, permissions, registry, nullptr);
  }

  ToolRegistry registry;
  firmius::shared::IHost *hostRaw = nullptr;
  std::unique_ptr<Agent> agent;

  std::string callTool(const std::string &name, const std::string &jsonArgs) {
    rapidjson::Document doc;
    doc.Parse(jsonArgs.c_str());
    ToolContext ctx{*hostRaw, *agent, "test_call"};
    auto res = registry.execute(name, doc, ctx);
    if (!res.success) {
      throw std::runtime_error("Tool " + name + " failed: " + res.error);
    }
    return res.data;
  }
};

TEST_F(ProcessInteractiveTest, PythonInteraction) {
  // Use a stdin-driven Python loop instead of REPL mode to avoid TTY-specific
  // behavior while still verifying interactive process IO.
  std::string spawnData = callTool(
      "process_spawn",
      R"({"command": "python3 -u -c 'import sys; exec(\"for line in sys.stdin:\\n    if line.strip() == \\\"__EXIT__\\\":\\n        break\\n    sys.stdout.write(line)\\n    sys.stdout.flush()\")'"})");
  rapidjson::Document spawnDoc;
  spawnDoc.Parse(spawnData.c_str());
  std::string pid = spawnDoc["process_id"].GetString();
  EXPECT_FALSE(pid.empty());

  // 2. Send input.
  callTool("process_input",
           R"({"process_id": ")" + pid +
               R"(", "input": "firmius_interactive_test\n"})");

  // 3. Wait for the output pattern to appear.
  std::string waitData = callTool(
      "process_wait",
      R"({"process_id": ")" + pid +
          R"(", "pattern": "firmius_interactive_test", "timeout_ms": 5000})");
  rapidjson::Document waitDoc;
  waitDoc.Parse(waitData.c_str());
  EXPECT_TRUE(waitDoc["patternFound"].GetBool());
  EXPECT_TRUE(waitDoc["isRunning"].GetBool());

  // 4. Ask the script to exit.
  callTool("process_input",
           R"({"process_id": ")" + pid +
               R"(", "input": "__EXIT__\n"})");

  // 5. Verify the process exits within a bounded timeout.
  std::string exitWaitData =
      callTool("process_wait",
               R"({"process_id": ")" + pid + R"(", "timeout_ms": 5000})");
  rapidjson::Document exitWaitDoc;
  exitWaitDoc.Parse(exitWaitData.c_str());
  EXPECT_FALSE(exitWaitDoc["isRunning"].GetBool());
}

TEST_F(ProcessInteractiveTest, PatternWaitTimeout) {
  // 1. Spawn a long running process
  std::string spawnData =
      callTool("process_spawn", R"({"command": "sleep 10"})");
  rapidjson::Document spawnDoc;
  spawnDoc.Parse(spawnData.c_str());
  std::string pid = spawnDoc["process_id"].GetString();

  // 2. Wait for a pattern that will never appear with a short timeout
  // process_wait should fail if timeout is reached
  rapidjson::Document doc;
  doc.Parse(
      R"({"process_id": "placeholder", "pattern": "never", "timeout_ms": 1000})");
  doc["process_id"].SetString(pid.c_str(), doc.GetAllocator());

  ToolContext ctx{*hostRaw, *agent, "test_call"};
  auto res = registry.execute("process_wait", doc, ctx);

  EXPECT_FALSE(res.success);
  EXPECT_TRUE(res.error.find("Timeout") != std::string::npos);
}

TEST(ProcessCancellationRegressionTest,
     DetachedRunWorkersDoNotResumeWhenNextRunStarts) {
  Panic::init();

  auto provider = std::make_shared<CancelProbeProvider>();
  auto &providerRegistry = ProviderRegistry::instance();
  if (!providerRegistry.getProvider(provider->getId())) {
    providerRegistry.registerProvider(provider);
  }

  ToolRegistry registry;
  auto probeState = std::make_shared<CancelProbeState>();
  registry.registerTool(std::make_unique<CancelProbeTool>(probeState));

  AgentContext ctx;
  ctx.config.providerId = provider->getId();
  ctx.config.modelId = "cancel-probe-model";
  ctx.environment.type = HostType::Local;
  ctx.environment.cwd = "/tmp";
  ctx.permissions.allowedScopes = {ToolScope::Process};
  ctx.permissions.allowedPaths = {"/tmp"};

  auto host = std::make_shared<LocalHost>();
  auto environment = std::make_shared<Environment>(
      host, ctx.environment.cwd, [](const StreamEvent &) {});
  auto permissions = std::make_shared<firmius::test::MockPermissions>();
  permissions->cwd_ = ctx.environment.cwd;
  permissions->allowedPaths_ = ctx.permissions.allowedPaths;

  Agent agent(ctx, environment, permissions, registry, nullptr);

  std::thread runA([&]() { agent.run("run A", [](const StreamEvent &) {}); });
  ASSERT_TRUE(waitForCondition(
      [&]() { return probeState->started.load() >= 1; },
      std::chrono::milliseconds(1500)));
  agent.interrupt();
  runA.join();

  probeState->nextRunStarted.store(true);
  agent.run("run B", [](const StreamEvent &) {});

  std::this_thread::sleep_for(std::chrono::milliseconds(900));

  EXPECT_EQ(probeState->sideEffectsAfterNextRun.load(), 0);
  EXPECT_GE(probeState->cancelledObserved.load(), 1);
  EXPECT_GE(provider->callCount(), 2);
}
