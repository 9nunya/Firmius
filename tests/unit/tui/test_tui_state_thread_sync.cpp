#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define private public
#include "TUIState.hpp"
#undef private

#include "harness/Harness.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::core::Harness;
using firmius::shared::AgentThinking;
using firmius::tui::StreamStateManager;
using firmius::tui::TuiState;

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

void resetTuiStateForTest(TuiState &state, Harness &harness) {
  state.harness_ = &harness;
  state.thread_ = {};
  state.focused_agent_id_.clear();
  state.history_.reset();
  state.stream_state_ = StreamStateManager{};
  state.title_model_.reset();
  state.status_model_.reset();
  state.input_model_.reset();
  state.agent_strip_model_.reset();
  state.plan_lane_model_.reset();
  state.todo_lane_model_.reset();
  state.context_lane_model_.reset();
  state.pending_refresh_flags_ = 0;
  state.screen_ = nullptr;
  state.view_mode_ = TuiState::ViewMode::Welcome;
}

TEST(TuiStateThreadSyncTest, PreserveLiveStateSyncKeepsStreamingTimeline) {
  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.stream_state_.handleAgentThinking(
      AgentThinking{"agent-1", "still streaming", ""});
  ASSERT_EQ(state.stream_state_.getTimeline().size(), 1u);

  state.syncCurrentThreadMetadataFromHarness(true);

  EXPECT_EQ(state.thread_.threadId, threadId);
  EXPECT_EQ(state.getViewMode(), TuiState::ViewMode::Chat);
  ASSERT_EQ(state.stream_state_.getTimeline().size(), 1u);
  EXPECT_EQ(state.stream_state_.getTimeline().front().message,
            "still streaming");
}

TEST(TuiStateThreadSyncTest, FullThreadSyncClearsStreamingTimeline) {
  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.stream_state_.handleAgentThinking(
      AgentThinking{"agent-1", "ephemeral thinking", ""});
  ASSERT_EQ(state.stream_state_.getTimeline().size(), 1u);

  state.syncCurrentThreadMetadataFromHarness(false);

  EXPECT_EQ(state.thread_.threadId, threadId);
  EXPECT_TRUE(state.stream_state_.getTimeline().empty());
}

TEST(TuiStateThreadSyncTest, ThinkingEventTriggersTranscriptRefreshSignal) {
  auto &harness = Harness::instance();
  harness.init();

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);

  auto probe = ftxui::Make<TranscriptProbeComponent>();
  auto *probe_ptr = probe.get();
  state.chat_component_ = probe;

  state.handleAppEvent(AgentThinking{"agent-1", "delta", ""});

  EXPECT_EQ(probe_ptr->transcript_changed_events, 1);
}

} // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  std::_Exit(result == 0 ? 0 : 1);
}
