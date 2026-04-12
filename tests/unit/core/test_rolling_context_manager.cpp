#include "agents/RollingContextManager.hpp"
#include "persistence/ThreadManager.hpp"
#include "IProvider.hpp"
#include "providers/ProviderRegistry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include <functional>
using namespace firmius::core;
using namespace firmius::shared;

namespace {

class DummyProvider final : public firmius::provider::IProvider {
public:
  std::string getId() const override { return "dummy"; }
  void stream(const AgentHistory &, const firmius::provider::ProviderOptions &,
              std::function<void(const StreamEvent &)>) override {}
  std::vector<ModelInfo> listModels() override { return {}; }
  ModelInfo getModelInfo(const std::string &) override { return {}; }
  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)>,
                       std::atomic<bool> *) override {}
  firmius::provider::ProviderType getProviderType() const override {
    return firmius::provider::ProviderType::APIKey;
  }
};

class RecordingSummaryProvider final : public firmius::provider::IProvider {
public:
  using GenerateHook = std::function<void()>;

  explicit RecordingSummaryProvider(std::string id) : id_(std::move(id)) {}

  std::string getId() const override { return id_; }

  void stream(const AgentHistory &, const firmius::provider::ProviderOptions &,
              std::function<void(const StreamEvent &)>) override {}

  std::vector<ModelInfo> listModels() override { return {}; }

  ModelInfo getModelInfo(const std::string &) override {
    ModelInfo info;
    info.contextWindow = 128000;
    return info;
  }

  void generateSummary(const std::string &, const AgentHistory &,
                       const std::string &,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> *) override {
    if (onGenerate_) {
      onGenerate_();
    }
    if (!summaryText_.empty()) {
      onEvent(TextChunk{summaryText_});
    }
  }

  firmius::provider::ProviderType getProviderType() const override {
    return firmius::provider::ProviderType::APIKey;
  }

  void setSummaryText(std::string text) { summaryText_ = std::move(text); }
  void setOnGenerate(GenerateHook hook) { onGenerate_ = std::move(hook); }

private:
  std::string id_;
  std::string summaryText_;
  GenerateHook onGenerate_;
};

class RollingContextManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char *existingHome = std::getenv("HOME");
    if (existingHome) {
      originalHome_ = existingHome;
    }
    tempHome_ = std::filesystem::temp_directory_path() /
                ("firmius_rolling_ctx_" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
    std::filesystem::create_directories(tempHome_);
    setenv("HOME", tempHome_.c_str(), 1);
    std::filesystem::create_directories(tempHome_ / ".firmius" / "threads");
    tm_ = std::make_unique<ThreadManager>(
        (tempHome_ / ".firmius" / "threads").string());

    ThreadMetadata metadata;
    metadata.title = "Rolling Memory";
    metadata.hostOptions.type = HostType::Local;
    metadata.cwd = "/work";
    metadata.leadPersona = "lead";
    threadId_ = tm_->createThread(metadata);
  }

  void TearDown() override {
    if (originalHome_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", originalHome_.c_str(), 1);
    }
    std::filesystem::remove_all(tempHome_);
  }

  AgentTurn makeTurn(const std::string &turnId, Role role,
                     const std::string &text) {
    AgentTurn turn;
    turn.turnId = turnId;
    Message message;
    message.role = role;
    message.timestamp = 1;
    message.content.push_back(TextContent{text});
    turn.messages.push_back(std::move(message));
    turn.metrics.tokens.contextSize = 1000;
    return turn;
  }

  const NoticeContent *findRollingNoticeByLifecycle(
      const std::vector<AgentTurn> &events, const std::string &eventKind,
      const std::string &lifecycle) {
    for (const auto &turn : events) {
      for (const auto &msg : turn.messages) {
        for (const auto &part : msg.content) {
          const auto *notice = std::get_if<NoticeContent>(&part);
          if (!notice || !notice->rollingMetadata.has_value()) {
            continue;
          }
          const auto &meta = notice->rollingMetadata.value();
          if (meta.eventKind == eventKind && meta.lifecycle == lifecycle) {
            return notice;
          }
        }
      }
    }
    return nullptr;
  }

  AgentContext makeContext() {
    AgentContext ctx;
    ctx.identity.id = "agent-1";
    ctx.history = std::make_shared<AgentHistory>();
    ctx.history->threadId = threadId_;
    ctx.config.providerId = "missing-provider";
    ctx.config.modelId = "missing-model";
    ctx.config.rollingMemory.enabled = true;
    ctx.config.rollingMemory.mode = "rolling_forever";
    ctx.config.rollingMemory.preset = "balanced";
    ctx.aggregateMetrics.tokens.contextSize = 90000;
    ctx.history->turns.push_back(makeTurn("bootstrap-system", Role::System, "sys"));
    ctx.history->turns.push_back(makeTurn("user-1", Role::User, "Need parser fix"));
    ctx.history->turns.push_back(makeTurn("assistant-2", Role::Assistant, "Investigating"));
    ctx.history->turns.push_back(makeTurn("tools-3", Role::ToolResult, "tool output"));
    ctx.history->turns.push_back(makeTurn("user-4", Role::User, "Still broken"));
    ctx.history->turns.push_back(makeTurn("assistant-5", Role::Assistant, "Patched Parser.cpp"));
    return ctx;
  }

  std::filesystem::path tempHome_;
  std::string originalHome_;
  std::unique_ptr<ThreadManager> tm_;
  std::string threadId_;
};

TEST_F(RollingContextManagerTest, EnabledForRollingForeverMode) {
  auto ctx = makeContext();
  EXPECT_TRUE(RollingContextManager::isEnabled(ctx));
}

TEST_F(RollingContextManagerTest, DisabledWhenRollingMemoryToggleOff) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.enabled = false;
  EXPECT_FALSE(RollingContextManager::isEnabled(ctx));
}

TEST_F(RollingContextManagerTest, DisabledWhenModeIsLegacyCompaction) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.mode = "legacy_compaction";
  EXPECT_FALSE(RollingContextManager::isEnabled(ctx));
}

TEST_F(RollingContextManagerTest, DisabledWhenModeIsDisabled) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.mode = "disabled";
  EXPECT_FALSE(RollingContextManager::isEnabled(ctx));
}

TEST_F(RollingContextManagerTest, EnabledForBenchmarkThreads) {
  auto ctx = makeContext();
  auto metadata = tm_->getMetadata(threadId_);
  metadata.isBenchmarkRun = true;
  metadata.benchmarkId = "swebench";
  metadata.benchmarkTaskId = "task-1";
  tm_->updateMetadata(threadId_, metadata);
  EXPECT_TRUE(RollingContextManager::isEnabled(ctx));
}

TEST_F(RollingContextManagerTest, BalancedPresetUsesExpectedRatios) {
  auto ctx = makeContext();
  const auto resolved = RollingContextManager::resolveThresholds(ctx);
  EXPECT_TRUE(resolved.enabled);
  EXPECT_EQ(resolved.preset, "balanced");
  EXPECT_FLOAT_EQ(resolved.targetOccupancyRatio, 0.57f);
  EXPECT_FLOAT_EQ(resolved.bufferOccupancyRatio, 0.47f);
  EXPECT_FLOAT_EQ(resolved.emergencyOccupancyRatio, 0.66f);
}

TEST_F(RollingContextManagerTest, AggressivePresetUsesExpectedRatios) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.preset = "aggressive";
  const auto resolved = RollingContextManager::resolveThresholds(ctx);
  EXPECT_FLOAT_EQ(resolved.targetOccupancyRatio, 0.48f);
  EXPECT_FLOAT_EQ(resolved.bufferOccupancyRatio, 0.38f);
  EXPECT_FLOAT_EQ(resolved.emergencyOccupancyRatio, 0.57f);
}

TEST_F(RollingContextManagerTest, ExtendedPresetUsesExpectedRatios) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.preset = "extended";
  const auto resolved = RollingContextManager::resolveThresholds(ctx);
  EXPECT_FLOAT_EQ(resolved.targetOccupancyRatio, 0.68f);
  EXPECT_FLOAT_EQ(resolved.bufferOccupancyRatio, 0.58f);
  EXPECT_FLOAT_EQ(resolved.emergencyOccupancyRatio, 0.77f);
}

TEST_F(RollingContextManagerTest, CustomPresetUsesConfiguredRatios) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.preset = "custom";
  ctx.config.rollingMemory.targetOccupancyRatio = 0.51f;
  ctx.config.rollingMemory.bufferOccupancyRatio = 0.41f;
  ctx.config.rollingMemory.emergencyOccupancyRatio = 0.61f;
  const auto resolved = RollingContextManager::resolveThresholds(ctx);
  EXPECT_FLOAT_EQ(resolved.targetOccupancyRatio, 0.51f);
  EXPECT_FLOAT_EQ(resolved.bufferOccupancyRatio, 0.41f);
  EXPECT_FLOAT_EQ(resolved.emergencyOccupancyRatio, 0.61f);
}

TEST_F(RollingContextManagerTest, ThresholdsFallbackToDefaultContextWindow) {
  auto ctx = makeContext();
  const auto resolved = RollingContextManager::resolveThresholds(ctx);
  EXPECT_EQ(resolved.contextWindow, 128000u);
}

TEST_F(RollingContextManagerTest, BufferThresholdStaysBelowTargetThreshold) {
  auto ctx = makeContext();
  const auto resolved = RollingContextManager::resolveThresholds(ctx);
  EXPECT_LT(resolved.bufferThresholdTokens, resolved.targetThresholdTokens);
}

TEST_F(RollingContextManagerTest, EmergencyThresholdStaysAboveTargetThreshold) {
  auto ctx = makeContext();
  const auto resolved = RollingContextManager::resolveThresholds(ctx);
  EXPECT_GT(resolved.emergencyThresholdTokens, resolved.targetThresholdTokens);
}

TEST_F(RollingContextManagerTest, RetainedTailUsesMinimumFloor) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.minimumRetainedTailTokens = 32000;
  const auto resolved = RollingContextManager::resolveThresholds(ctx);
  EXPECT_GE(resolved.retainedTailTokens, 32000u);
}

TEST_F(RollingContextManagerTest, FilterHistoryKeepsAllWithoutState) {
  auto ctx = makeContext();
  const auto filtered =
      RollingContextManager::filterHistoryForRequest(ctx, *ctx.history);
  EXPECT_EQ(filtered.turns.size(), ctx.history->turns.size());
}

TEST_F(RollingContextManagerTest, FilterHistoryRemovesCoveredOldTurns) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.preset = "custom";
  ctx.config.rollingMemory.retainTailRatio = 0.0f;
  ctx.config.rollingMemory.minimumRetainedTailTokens = 1;
  RollingMemoryState state;
  state.threadId = threadId_;
  state.agentId = ctx.identity.id;
  RollingMemoryChunk chunk;
  chunk.chunkId = "obs-1";
  chunk.active = true;
  chunk.sourceTurnIds = {"user-1", "assistant-2"};
  state.observationChunks.push_back(chunk);
  tm_->writeRollingMemoryState(threadId_, ctx.identity.id, state);

  const auto filtered =
      RollingContextManager::filterHistoryForRequest(ctx, *ctx.history);
  EXPECT_LT(filtered.turns.size(), ctx.history->turns.size());
}

TEST_F(RollingContextManagerTest, FilterHistoryPreservesRecentTailEvenIfCovered) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.minimumRetainedTailTokens = 100000;
  RollingMemoryState state;
  state.threadId = threadId_;
  state.agentId = ctx.identity.id;
  RollingMemoryChunk chunk;
  chunk.chunkId = "obs-1";
  chunk.active = true;
  chunk.sourceTurnIds = {"user-4", "assistant-5"};
  state.observationChunks.push_back(chunk);
  tm_->writeRollingMemoryState(threadId_, ctx.identity.id, state);

  const auto filtered =
      RollingContextManager::filterHistoryForRequest(ctx, *ctx.history);
  EXPECT_EQ(filtered.turns.size(), ctx.history->turns.size());
}

TEST_F(RollingContextManagerTest, MemoryOverlayEmptyWhenNoActiveChunks) {
  auto ctx = makeContext();
  EXPECT_TRUE(RollingContextManager::buildMemoryOverlay(ctx).empty());
}

TEST_F(RollingContextManagerTest, MemoryOverlayIncludesActiveObservation) {
  auto ctx = makeContext();
  RollingMemoryState state;
  state.threadId = threadId_;
  state.agentId = ctx.identity.id;
  RollingMemoryChunk chunk;
  chunk.chunkId = "obs-1";
  chunk.active = true;
  chunk.sourceStartTurnId = "user-1";
  chunk.sourceEndTurnId = "assistant-2";
  chunk.summary = "Parser crash investigation";
  state.observationChunks.push_back(chunk);
  tm_->writeRollingMemoryState(threadId_, ctx.identity.id, state);

  const auto text = RollingContextManager::buildMemoryOverlay(ctx);
  EXPECT_NE(text.find("Active observations"), std::string::npos);
  EXPECT_NE(text.find("Parser crash investigation"), std::string::npos);
}

TEST_F(RollingContextManagerTest, MemoryOverlayIncludesActiveReflection) {
  auto ctx = makeContext();
  RollingMemoryState state;
  state.threadId = threadId_;
  state.agentId = ctx.identity.id;
  RollingMemoryChunk chunk;
  chunk.chunkId = "refl-1";
  chunk.active = true;
  chunk.sourceStartTurnId = "user-1";
  chunk.sourceEndTurnId = "assistant-5";
  chunk.summary = "Earlier parser work summary";
  state.reflectionChunks.push_back(chunk);
  tm_->writeRollingMemoryState(threadId_, ctx.identity.id, state);

  const auto text = RollingContextManager::buildMemoryOverlay(ctx);
  EXPECT_NE(text.find("Active reflections"), std::string::npos);
  EXPECT_NE(text.find("Earlier parser work summary"), std::string::npos);
}

TEST_F(RollingContextManagerTest, StatusOverlayIncludesThresholds) {
  auto ctx = makeContext();
  const auto text = RollingContextManager::buildStatusOverlay(ctx);
  EXPECT_NE(text.find("Buffer threshold"), std::string::npos);
  EXPECT_NE(text.find("Target threshold"), std::string::npos);
  EXPECT_NE(text.find("Emergency threshold"), std::string::npos);
}

TEST_F(RollingContextManagerTest, StatusOverlayIncludesDefaultMaintenanceModelLabels) {
  auto ctx = makeContext();
  const auto text = RollingContextManager::buildStatusOverlay(ctx);
  EXPECT_NE(text.find("Observer: missing-provider/missing-model"), std::string::npos);
  EXPECT_NE(text.find("Reflector: missing-provider/missing-model"), std::string::npos);
}

TEST_F(RollingContextManagerTest, MaintainCreatesBufferedObservationChunk) {
  auto ctx = makeContext();
  ctx.aggregateMetrics.tokens.contextSize = 90000;
  ctx.config.rollingMemory.preset = "custom";
  ctx.config.rollingMemory.retainTailRatio = 0.0f;
  ctx.config.rollingMemory.minimumChunkTokens = 1;
  ctx.config.rollingMemory.minimumRetainedTailTokens = 1;
  std::vector<AgentTurn> events;
  DummyProvider dummyProvider;

  RollingContextManager::maintain(
      ctx, dummyProvider, [&](const AgentTurn &turn) { events.push_back(turn); });
  const auto state = tm_->loadRollingMemoryState(threadId_, ctx.identity.id);
  ASSERT_FALSE(state.observationChunks.empty());
  EXPECT_TRUE(state.observationChunks.front().buffered ||
              state.observationChunks.front().active);
  EXPECT_FALSE(events.empty());
}

TEST_F(RollingContextManagerTest, MaintainActivatesBufferedObservationAtTargetThreshold) {
  auto ctx = makeContext();
  RollingMemoryState state;
  state.threadId = threadId_;
  state.agentId = ctx.identity.id;
  RollingMemoryChunk chunk;
  chunk.chunkId = "obs-1";
  chunk.buffered = true;
  chunk.sourceTurnIds = {"user-1", "assistant-2"};
  state.observationChunks.push_back(chunk);
  tm_->writeRollingMemoryState(threadId_, ctx.identity.id, state);

  DummyProvider dummyProvider;

  RollingContextManager::maintain(ctx, dummyProvider,
                                  [&](const AgentTurn &) {});
  const auto reloaded = tm_->loadRollingMemoryState(threadId_, ctx.identity.id);
  EXPECT_TRUE(reloaded.observationChunks.front().active);
  EXPECT_FALSE(reloaded.observationChunks.front().buffered);
}

TEST_F(RollingContextManagerTest, MaintainHonorsEmitEventTurnsFalse) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.emitEventTurns = false;
  std::vector<AgentTurn> events;
  DummyProvider dummyProvider;
  RollingContextManager::maintain(
      ctx, dummyProvider, [&](const AgentTurn &turn) { events.push_back(turn); });
  EXPECT_TRUE(events.empty());
}

TEST_F(RollingContextManagerTest, MaintainRunsForBenchmarkThread) {
  auto ctx = makeContext();
  auto metadata = tm_->getMetadata(threadId_);
  metadata.isBenchmarkRun = true;
  metadata.benchmarkId = "swebench";
  metadata.benchmarkTaskId = "task-1";
  tm_->updateMetadata(threadId_, metadata);
  ctx.config.rollingMemory.preset = "custom";
  ctx.config.rollingMemory.retainTailRatio = 0.0f;
  ctx.config.rollingMemory.minimumChunkTokens = 1;
  ctx.config.rollingMemory.minimumRetainedTailTokens = 1;
  DummyProvider dummyProvider;
  RollingContextManager::maintain(ctx, dummyProvider,
                                  [&](const AgentTurn &) {});
  const auto state = tm_->loadRollingMemoryState(threadId_, ctx.identity.id);
  EXPECT_FALSE(state.observationChunks.empty());
}

TEST_F(RollingContextManagerTest, MaintainCanCreateReflectionFromActiveObservations) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.minimumChunkTokens = 1;
  ctx.config.rollingMemory.reflectionOccupancyRatio = 0.02f;
  ctx.config.rollingMemory.preset = "custom";
  RollingMemoryState state;
  state.threadId = threadId_;
  state.agentId = ctx.identity.id;
  for (int i = 0; i < 3; ++i) {
    RollingMemoryChunk chunk;
    chunk.chunkId = "obs-" + std::to_string(i);
    chunk.active = true;
    chunk.sourceStartTurnId = "user-1";
    chunk.sourceEndTurnId = "assistant-2";
    chunk.summary = std::string(6000, 'a' + i);
    chunk.summaryTokens = 1000;
    chunk.sourceTurnIds = {"user-1", "assistant-2"};
    state.observationChunks.push_back(chunk);
  }
  tm_->writeRollingMemoryState(threadId_, ctx.identity.id, state);
  DummyProvider dummyProvider;
  RollingContextManager::maintain(ctx, dummyProvider,
                                  [&](const AgentTurn &) {});
  const auto reloaded = tm_->loadRollingMemoryState(threadId_, ctx.identity.id);
  ASSERT_FALSE(reloaded.reflectionChunks.empty());
  EXPECT_TRUE(reloaded.reflectionChunks.front().active);
}

TEST_F(RollingContextManagerTest, StatusOverlayReflectsCountsFromState) {
  auto ctx = makeContext();
  RollingMemoryState state;
  state.threadId = threadId_;
  state.agentId = ctx.identity.id;
  RollingMemoryChunk buffered;
  buffered.chunkId = "obs-1";
  buffered.summary = "a";
  buffered.buffered = true;
  state.observationChunks.push_back(buffered);
  RollingMemoryChunk active;
  active.chunkId = "obs-2";
  active.summary = "b";
  active.active = true;
  state.observationChunks.push_back(active);
  RollingMemoryChunk reflection;
  reflection.chunkId = "refl-1";
  reflection.summary = "c";
  reflection.active = true;
  state.reflectionChunks.push_back(reflection);
  tm_->writeRollingMemoryState(threadId_, ctx.identity.id, state);
  const auto text = RollingContextManager::buildStatusOverlay(ctx);
  EXPECT_NE(text.find("Active reflections: 1"), std::string::npos);
  EXPECT_NE(text.find("Active observations: 1"), std::string::npos);
  EXPECT_NE(text.find("Buffered observations: 1"), std::string::npos);
}

TEST_F(RollingContextManagerTest, MaintainDoesNotSummarizeBootstrapTurn) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.preset = "custom";
  ctx.config.rollingMemory.retainTailRatio = 0.0f;
  ctx.config.rollingMemory.minimumChunkTokens = 1;
  ctx.config.rollingMemory.minimumRetainedTailTokens = 1;
  DummyProvider dummyProvider;
  RollingContextManager::maintain(ctx, dummyProvider,
                                  [&](const AgentTurn &) {});
  const auto state = tm_->loadRollingMemoryState(threadId_, ctx.identity.id);
  ASSERT_FALSE(state.observationChunks.empty());
  EXPECT_EQ(std::find(state.observationChunks.front().sourceTurnIds.begin(),
                      state.observationChunks.front().sourceTurnIds.end(),
                      "bootstrap-system"),
            state.observationChunks.front().sourceTurnIds.end());
}

TEST_F(RollingContextManagerTest, MemoryOverlayReturnsEmptyForDisabledMode) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.mode = "disabled";
  EXPECT_TRUE(RollingContextManager::buildMemoryOverlay(ctx).empty());
}

TEST_F(RollingContextManagerTest, StatusOverlayReturnsEmptyForDisabledMode) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.mode = "disabled";
  EXPECT_TRUE(RollingContextManager::buildStatusOverlay(ctx).empty());
}

TEST_F(RollingContextManagerTest, MaintainObservationPersistsInFlightAndStructuredEvents) {
  auto ctx = makeContext();
  ctx.aggregateMetrics.tokens.contextSize = 70000;
  ctx.config.rollingMemory.preset = "custom";
  ctx.config.rollingMemory.bufferOccupancyRatio = 0.20f;
  ctx.config.rollingMemory.targetOccupancyRatio = 0.30f;
  ctx.config.rollingMemory.emergencyOccupancyRatio = 0.40f;
  ctx.config.rollingMemory.reflectionOccupancyRatio = 0.20f;
  ctx.config.rollingMemory.retainTailRatio = 0.0f;
  ctx.config.rollingMemory.minimumChunkTokens = 1;
  ctx.config.rollingMemory.minimumRetainedTailTokens = 1;

  auto observer = std::make_shared<RecordingSummaryProvider>(
      "rolling-observer-structured");
  observer->setSummaryText(
      "<summary>Observed parser failure details</summary>"
      "<current_task>Fix parser</current_task>"
      "<suggested_response>Apply parser patch</suggested_response>");

  bool sawInFlightDuringGenerate = false;
  observer->setOnGenerate([&]() {
    const auto during = tm_->loadRollingMemoryState(threadId_, ctx.identity.id);
    sawInFlightDuringGenerate = during.observationInFlight;
  });
  firmius::provider::ProviderRegistry::instance().registerProvider(observer);
  ctx.config.rollingMemory.observer.enabled = true;
  ctx.config.rollingMemory.observer.providerId = observer->getId();
  ctx.config.rollingMemory.observer.modelId = "obs-model";

  std::vector<AgentTurn> events;
  DummyProvider dummyProvider;
  RollingContextManager::maintain(
      ctx, dummyProvider, [&](const AgentTurn &turn) { events.push_back(turn); });

  const auto state = tm_->loadRollingMemoryState(threadId_, ctx.identity.id);
  EXPECT_TRUE(sawInFlightDuringGenerate);
  EXPECT_FALSE(state.observationInFlight);

  const NoticeContent *start =
      findRollingNoticeByLifecycle(events, "observation", "start");
  ASSERT_NE(start, nullptr);
  ASSERT_TRUE(start->rollingMetadata.has_value());
  EXPECT_TRUE(start->rollingMetadata->modelLabel.has_value());
  EXPECT_EQ(start->rollingMetadata->modelLabel.value(), observer->getId() + "/obs-model");
  EXPECT_TRUE(start->rollingMetadata->sourceTokens.has_value());
  EXPECT_FALSE(start->rollingMetadata->summaryTokens.has_value());

  const NoticeContent *complete =
      findRollingNoticeByLifecycle(events, "observation", "complete");
  ASSERT_NE(complete, nullptr);
  ASSERT_TRUE(complete->rollingMetadata.has_value());
  EXPECT_TRUE(complete->rollingMetadata->summaryTokens.has_value());
  EXPECT_TRUE(complete->rollingMetadata->savedTokens.has_value());
}

TEST_F(RollingContextManagerTest, MaintainReflectionPersistsInFlightAndStructuredEvents) {
  auto ctx = makeContext();
  ctx.config.rollingMemory.minimumChunkTokens = 1;
  ctx.config.rollingMemory.reflectionOccupancyRatio = 0.02f;
  ctx.config.rollingMemory.preset = "custom";

  RollingMemoryState state;
  state.threadId = threadId_;
  state.agentId = ctx.identity.id;
  for (int i = 0; i < 3; ++i) {
    RollingMemoryChunk chunk;
    chunk.chunkId = "obs-pre-" + std::to_string(i);
    chunk.active = true;
    chunk.sourceStartTurnId = "user-1";
    chunk.sourceEndTurnId = "assistant-2";
    chunk.summary = std::string(6000, 'a' + i);
    chunk.summaryTokens = 1000;
    chunk.sourceTurnIds = {"user-1", "assistant-2"};
    state.observationChunks.push_back(chunk);
  }
  tm_->writeRollingMemoryState(threadId_, ctx.identity.id, state);

  auto reflector = std::make_shared<RecordingSummaryProvider>(
      "rolling-reflector-structured");
  reflector->setSummaryText(
      "<summary>Reflected parser history</summary>"
      "<current_task>Continue fix</current_task>"
      "<suggested_response>Confirm patch</suggested_response>");

  bool sawInFlightDuringGenerate = false;
  reflector->setOnGenerate([&]() {
    const auto during = tm_->loadRollingMemoryState(threadId_, ctx.identity.id);
    sawInFlightDuringGenerate = during.reflectionInFlight;
  });
  firmius::provider::ProviderRegistry::instance().registerProvider(reflector);
  ctx.config.rollingMemory.reflector.enabled = true;
  ctx.config.rollingMemory.reflector.providerId = reflector->getId();
  ctx.config.rollingMemory.reflector.modelId = "refl-model";

  std::vector<AgentTurn> events;
  DummyProvider dummyProvider;
  RollingContextManager::maintain(
      ctx, dummyProvider, [&](const AgentTurn &turn) { events.push_back(turn); });

  const auto reloaded = tm_->loadRollingMemoryState(threadId_, ctx.identity.id);
  EXPECT_TRUE(sawInFlightDuringGenerate);
  EXPECT_FALSE(reloaded.reflectionInFlight);
  ASSERT_FALSE(reloaded.reflectionChunks.empty());

  const NoticeContent *start =
      findRollingNoticeByLifecycle(events, "reflection", "start");
  ASSERT_NE(start, nullptr);
  ASSERT_TRUE(start->rollingMetadata.has_value());
  EXPECT_TRUE(start->rollingMetadata->sourceChunkCount.has_value());
  EXPECT_FALSE(start->rollingMetadata->summaryTokens.has_value());

  const NoticeContent *complete =
      findRollingNoticeByLifecycle(events, "reflection", "complete");
  ASSERT_NE(complete, nullptr);
  ASSERT_TRUE(complete->rollingMetadata.has_value());
  EXPECT_TRUE(complete->rollingMetadata->summaryTokens.has_value());
  EXPECT_TRUE(complete->rollingMetadata->savedTokens.has_value());
}

} // namespace
