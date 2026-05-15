#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#define private public
#include "TUIState.hpp"
#undef private

#include "AgentRegistry.hpp"
#include "UserPreferences.hpp"
#include "components/InputBar.hpp"
#include "components/PlanLane.hpp"
#include "components/StatusBar.hpp"
#include "components/TitleBar.hpp"
#include "harness/Harness.hpp"
#include "../mocks/MockAgent.hpp"

#include <ftxui/component/component.hpp>
#include <gtest/gtest.h>

namespace {

using firmius::core::AgentRegistry;
using firmius::core::Harness;
using firmius::shared::AgentContext;
using firmius::shared::AgentInterrupted;
using firmius::shared::AgentProcessOutput;
using firmius::shared::AgentSpawned;
using firmius::shared::ThreadChanged;
using firmius::shared::ThreadMetadata;
using firmius::shared::ThreadPermissionMode;
using firmius::tui::TuiState;
using firmius::test::MockAgent;

class TranscriptProbeComponent : public ftxui::ComponentBase {
public:
  int transcript_changed_events = 0;

  bool OnEvent(ftxui::Event event) override {
    if (event == ftxui::Event::Special("TranscriptChanged")) {
      ++transcript_changed_events;
      return true;
    }
    return false;
  }
};

class ScopedHomeOverride {
public:
  explicit ScopedHomeOverride(std::filesystem::path home) : home_(std::move(home)) {
    if (const char *current = std::getenv("HOME")) {
      original_ = current;
    }
    std::filesystem::remove_all(home_);
    std::filesystem::create_directories(home_ / ".firmius");
    setenv("HOME", home_.c_str(), 1);
  }

  ~ScopedHomeOverride() {
    if (original_) {
      setenv("HOME", original_->c_str(), 1);
    } else {
      unsetenv("HOME");
    }
    std::filesystem::remove_all(home_);
  }

  const std::filesystem::path &path() const { return home_; }

private:
  std::filesystem::path home_;
  std::optional<std::string> original_;
};

class ScopedRegisteredAgent {
public:
  ScopedRegisteredAgent(std::string id, AgentContext context) : id_(std::move(id)) {
    context.identity.id = id_;
    AgentRegistry::instance().registerAgent(
        id_, std::make_shared<MockAgent>(std::move(context)));
  }

  ~ScopedRegisteredAgent() { AgentRegistry::instance().unregisterAgent(id_); }

private:
  std::string id_;
};

void resetTuiStateForTest(TuiState &state, Harness &harness) {
  state.harness_ = &harness;
  state.thread_ = {};
  state.focused_agent_id_.clear();
  state.focused_process_id_.clear();
  state.history_.reset();
  state.chat_component_.reset();
  state.title_model_.reset();
  state.status_model_.reset();
  state.input_model_.reset();
  state.agent_strip_model_.reset();
  state.plan_lane_model_.reset();
  state.todo_lane_model_.reset();
  state.context_lane_model_.reset();
}

TEST(TuiStateThreadSyncTest, PreserveLiveStateSyncKeepsStreamingTimeline) {
  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.thread_.threadId = threadId;
  state.focused_agent_id_ = "lead-1";

  auto probe = ftxui::Make<TranscriptProbeComponent>();
  auto *probe_ptr = probe.get();
  state.chat_component_ = probe;

  state.syncCurrentThreadMetadataFromHarness(true);

  EXPECT_EQ(state.thread_.threadId, threadId);
  EXPECT_EQ(state.getViewMode(), TuiState::ViewMode::Chat);
  EXPECT_GE(probe_ptr->transcript_changed_events, 1);
}

TEST(TuiStateThreadSyncTest, FullThreadSyncClearsStreamingTimeline) {
  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.thread_.threadId = threadId;
  state.focused_agent_id_ = "lead-1";

  auto probe = ftxui::Make<TranscriptProbeComponent>();
  auto *probe_ptr = probe.get();
  state.chat_component_ = probe;

  state.syncCurrentThreadMetadataFromHarness(false);

  EXPECT_EQ(state.thread_.threadId, threadId);
  EXPECT_EQ(state.getViewMode(), TuiState::ViewMode::Chat);
  EXPECT_GE(probe_ptr->transcript_changed_events, 1);
}

TEST(TuiStateThreadSyncTest, FocusAgentUpdatesHarnessAndState) {
  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  AgentContext leadContext;
  leadContext.identity.name = "lead";
  leadContext.identity.role = "lead";
  leadContext.history = std::make_shared<firmius::shared::AgentHistory>();
  leadContext.history->threadId = threadId;
  ScopedRegisteredAgent lead("lead-1", leadContext);

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.thread_.threadId = threadId;

  ASSERT_TRUE(state.focusAgent("lead-1"));
  EXPECT_EQ(state.focused_agent_id_, "lead-1");
  EXPECT_EQ(harness.focusedAgentId(), "lead-1");
}

TEST(TuiStateThreadSyncTest, ThreadChangedBackfillsHarnessFocus) {
  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  AgentContext leadContext;
  leadContext.identity.name = "lead";
  leadContext.identity.role = "lead";
  leadContext.history = std::make_shared<firmius::shared::AgentHistory>();
  leadContext.history->threadId = threadId;
  ScopedRegisteredAgent lead("lead-2", leadContext);

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.thread_.threadId = threadId;
  state.focused_agent_id_.clear();

  ThreadMetadata metadata;
  metadata.threadId = threadId;
  metadata.title = "title";
  state.handleAppEvent(ThreadChanged{threadId, metadata});

  EXPECT_EQ(state.focused_agent_id_, "lead-2");
  EXPECT_EQ(harness.focusedAgentId(), "lead-2");
}

TEST(TuiStateThreadSyncTest, ApplyThreadOpenedPersistsFocusedAgent) {
  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  AgentContext leadContext;
  leadContext.identity.name = "lead";
  leadContext.identity.role = "lead";
  leadContext.history = std::make_shared<firmius::shared::AgentHistory>();
  leadContext.history->threadId = threadId;
  ScopedRegisteredAgent lead("lead-3", leadContext);

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);

  ThreadMetadata metadata;
  metadata.threadId = threadId;
  metadata.title = "New Thread";

  state.applyThreadOpened(metadata, "lead-3", true);

  EXPECT_EQ(state.focused_agent_id_, "lead-3");
  EXPECT_EQ(harness.focusedAgentId(), "lead-3");
}

TEST(TuiStateThreadSyncTest, SubagentSpawnDoesNotStealFocusFromParent) {
  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  AgentContext parentContext;
  parentContext.identity.name = "lead";
  parentContext.identity.role = "lead";
  parentContext.history = std::make_shared<firmius::shared::AgentHistory>();
  parentContext.history->threadId = threadId;

  AgentContext childContext;
  childContext.identity.name = "scout";
  childContext.identity.role = "scout";
  childContext.identity.parentId = "focus-parent";
  childContext.history = std::make_shared<firmius::shared::AgentHistory>();
  childContext.history->threadId = threadId;

  ScopedRegisteredAgent parent("focus-parent", parentContext);
  ScopedRegisteredAgent child("focus-child", childContext);

  ASSERT_TRUE(harness.setFocusedAgent("focus-parent"));

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.thread_.threadId = threadId;
  state.focused_agent_id_ = "focus-parent";

  state.handleAppEvent(AgentSpawned{"focus-child", "scout", "focus-parent",
                                    "scout", "Scout", true, "", "", 0});

  EXPECT_EQ(state.focused_agent_id_, "focus-parent");
  EXPECT_EQ(harness.focusedAgentId(), "focus-parent");
}

TEST(TuiStateThreadSyncTest, ProvisionalWelcomeThreadOpenDoesNotClearAlreadyEstablishedLeadFocus) {
  auto &harness = Harness::instance();
  harness.init();

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.initModels();
  state.setViewMode(TuiState::ViewMode::Chat);

  firmius::shared::ThreadMetadata metadata;
  metadata.threadId = "thread-1";
  metadata.title = "New Thread";

  AgentContext leadContext;
  leadContext.identity.name = "lead";
  leadContext.identity.role = "lead";
  leadContext.history = std::make_shared<firmius::shared::AgentHistory>();
  leadContext.history->threadId = "thread-1";
  ScopedRegisteredAgent lead("lead-1", leadContext);
  ASSERT_TRUE(harness.setFocusedAgent("lead-1"));

  state.focused_agent_id_ = "lead-1";

  state.applyThreadOpened(metadata, "", true);

  EXPECT_EQ(state.focused_agent_id_, "lead-1");
  EXPECT_EQ(harness.focusedAgentId(), "lead-1");
}

TEST(TuiStateThreadSyncTest, ProcessOutputEventTriggersTranscriptRefreshSignal) {
  auto &harness = Harness::instance();
  harness.init();

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);

  auto probe = ftxui::Make<TranscriptProbeComponent>();
  auto *probe_ptr = probe.get();
  state.chat_component_ = probe;

  state.handleAppEvent(
      AgentProcessOutput{"agent-1", "proc-1", "live chunk\n", false, false,
                         -1, 0.0, ""});

  EXPECT_EQ(probe_ptr->transcript_changed_events, 1);
}

TEST(TuiStateThreadSyncTest, AgentInterruptedTriggersTranscriptRefreshSignal) {
  auto &harness = Harness::instance();
  harness.init();

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);

  auto probe = ftxui::Make<TranscriptProbeComponent>();
  auto *probe_ptr = probe.get();
  state.chat_component_ = probe;

  state.handleAppEvent(AgentInterrupted{"agent-1", ""});

  EXPECT_EQ(probe_ptr->transcript_changed_events, 1);
}

TEST(TuiStateThreadSyncTest, InitAppliesPreferredPermissionModeToRequestThreads) {
  ScopedHomeOverride home(std::filesystem::temp_directory_path() /
                          "firmius_tui_thread_sync_home");

  firmius::tui::UserPreferences prefs;
  prefs.preferred_permission_mode = ThreadPermissionMode::AlwaysAllow;
  firmius::tui::saveUserPreferences(prefs);

  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  ThreadMetadata metadata;
  for (const auto &candidate : harness.listThreads()) {
    if (candidate.threadId == threadId) {
      metadata = candidate;
      break;
    }
  }
  ASSERT_EQ(metadata.threadId, threadId);
  ASSERT_EQ(metadata.permissionMode, ThreadPermissionMode::Request);

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.init(harness, metadata, "");

  EXPECT_EQ(state.thread_.permissionMode, ThreadPermissionMode::AlwaysAllow);
  EXPECT_EQ(harness.currentThreadPermissionMode(), ThreadPermissionMode::AlwaysAllow);
}

} // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  std::_Exit(result == 0 ? 0 : 1);
}
