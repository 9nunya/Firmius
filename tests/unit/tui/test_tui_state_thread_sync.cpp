#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define private public
#include "TUIState.hpp"
#undef private

#include "AgentRegistry.hpp"
#include "agents/hooks/HookState.hpp"
#include "agents/hooks/ScriptRuntime.hpp"
#include "harness/Harness.hpp"
#include "controllers/InputController.hpp"
#include "controllers/AppController.hpp"
#include "UserPreferences.hpp"
#include "../mocks/MockAgent.hpp"

#include <gtest/gtest.h>

namespace {

using firmius::core::Harness;
using firmius::core::AgentRegistry;
using firmius::shared::AgentContext;
using firmius::shared::AgentAccountSwitched;
using firmius::shared::AgentCompactionText;
using firmius::shared::AgentCompactionThinking;
using firmius::shared::AgentError;
using firmius::shared::AgentFinished;
using firmius::shared::AgentOutcome;
using firmius::shared::AgentThinking;
using firmius::shared::AgentInterrupted;
using firmius::shared::AgentProcessOutput;
using firmius::shared::AgentProcessSpawned;
using firmius::shared::AgentProviderWaiting;
using firmius::shared::AgentRetryFailed;
using firmius::shared::AgentRetrying;
using firmius::shared::AgentSpawned;
using firmius::shared::AgentText;
using firmius::shared::AgentToolCall;
using firmius::shared::AgentToolCallChunk;
using firmius::shared::InternalMessageQueued;
using firmius::shared::MessageQueued;
using firmius::test::MockAgent;
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

class ScopedHomeOverride {
public:
  explicit ScopedHomeOverride(const std::filesystem::path &home)
      : temp_home_(home) {
    if (const char *current = std::getenv("HOME")) {
      original_home_ = std::string(current);
    }
    std::filesystem::remove_all(temp_home_);
    std::filesystem::create_directories(temp_home_ / ".firmius");
    setenv("HOME", temp_home_.c_str(), 1);
  }

  ~ScopedHomeOverride() {
    if (original_home_.has_value()) {
      setenv("HOME", original_home_->c_str(), 1);
    } else {
      unsetenv("HOME");
    }
    std::filesystem::remove_all(temp_home_);
  }

  const std::filesystem::path &path() const { return temp_home_; }

private:
  std::filesystem::path temp_home_;
  std::optional<std::string> original_home_;
};

class ScopedEnvOverride {
public:
  ScopedEnvOverride(const char *name, const std::filesystem::path &value)
      : name_(name) {
    if (const char *current = std::getenv(name_)) {
      original_value_ = std::string(current);
    }
    setenv(name_, value.c_str(), 1);
  }

  ~ScopedEnvOverride() {
    if (original_value_.has_value()) {
      setenv(name_, original_value_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

private:
  const char *name_;
  std::optional<std::string> original_value_;
};

class ScopedEnvUnset {
public:
  explicit ScopedEnvUnset(const char *name) : name_(name) {
    if (const char *current = std::getenv(name_)) {
      original_value_ = std::string(current);
    }
    unsetenv(name_);
  }

  ~ScopedEnvUnset() {
    if (original_value_.has_value()) {
      setenv(name_, original_value_->c_str(), 1);
    }
  }

private:
  const char *name_;
  std::optional<std::string> original_value_;
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
  firmius::tui::TUIStore::instance().focused_agent_id.clear();
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
  state.deferred_ui_mutations_.clear();
  state.loading_message_.clear();
  state.loading_progress_ = -1.0f;
  state.loading_detail_.clear();
}

template <typename F> void runAcrossSkins(F &&fn) {
  for (const auto skin : {firmius::tui::SkinKind::Firmius,
                          firmius::tui::SkinKind::Claudex}) {
    SCOPED_TRACE(skin == firmius::tui::SkinKind::Claudex ? "Claudex"
                                                         : "Firmius");
    fn(skin);
  }
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


TEST(TuiStateThreadSyncTest, LoadingOverlayCarriesProgressAndDetailState) {
  auto &harness = Harness::instance();
  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);

  state.setLoadingMessage("Hydrating first frame…");
  state.setLoadingProgress(0.625f);
  state.setLoadingDetail("Phase 5/8 complete. Loaded in 125.000 ms.");

  EXPECT_EQ(state.loadingMessage(), "Hydrating first frame…");
  EXPECT_FLOAT_EQ(state.loadingProgress(), 0.625f);
  EXPECT_EQ(state.loadingDetail(), "Phase 5/8 complete. Loaded in 125.000 ms.");

  state.clearLoadingProgress();
  EXPECT_LT(state.loadingProgress(), 0.0f);
}
TEST(TuiStateThreadSyncTest, HookStatusLinesComeFromExternalLuaScript) {
  if (!firmius::core::hooks::ScriptRuntime::enabled()) {
    GTEST_SKIP() << "Luau hooks disabled in this build";
  }

  const auto tempRoot = std::filesystem::temp_directory_path() /
                        ("firmius_tui_hook_status_" +
                         std::to_string(static_cast<long long>(
                             std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count())));
  ScopedHomeOverride home(tempRoot / "home");
  ScopedEnvOverride hooksDir("FIRMIUS_HOOKS_DIR", tempRoot / "hooks");

  const auto scriptDir = tempRoot / "hooks" / "promise" / "skin";
  std::filesystem::create_directories(scriptDir);
  std::ofstream script(scriptDir / "status.lua");
  script << R"(local promise = state.read("thread", "promise")
if type(promise) ~= "table" or not promise.state then
  return ""
end
return "external-status " .. tostring(promise.state)
)";
  script.close();

  const std::string threadId = "thread-status-script";
  firmius::core::hooks::HookState::instance().bindThread(threadId);
  ASSERT_TRUE(firmius::core::hooks::HookState::instance().writeJson(
      firmius::core::hooks::HookState::Scope::Thread, "promise.state",
      R"("open")", "status-test"));

  auto &harness = Harness::instance();
  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.status_model_ = std::make_shared<firmius::tui::StatusBarModel>();
  state.thread_.threadId = threadId;

  state.updateStatusModel();

  ASSERT_NE(state.status_model_, nullptr);
  ASSERT_EQ(state.status_model_->hook_status_lines.size(), 1u);
  EXPECT_EQ(state.status_model_->hook_status_lines.front(),
            "external-status open");

  std::filesystem::remove_all(tempRoot);
}

TEST(TuiStateThreadSyncTest, HookStatusScriptsLoadFromInstalledPromptHooks) {
  if (!firmius::core::hooks::ScriptRuntime::enabled()) {
    GTEST_SKIP() << "Luau hooks disabled in this build";
  }

  const auto tempRoot = std::filesystem::temp_directory_path() /
                        ("firmius_tui_installed_hook_status_" +
                         std::to_string(static_cast<long long>(
                             std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count())));
  ScopedHomeOverride home(tempRoot / "home");
  ScopedEnvUnset hooksDir("FIRMIUS_HOOKS_DIR");

  const auto scriptDir =
      tempRoot / "home" / ".firmius" / "prompts" / "hooks" / "promise" /
      "skin";
  std::filesystem::create_directories(scriptDir);
  std::ofstream script(scriptDir / "status.lua");
  script << R"(local promise = state.read("thread", "promise")
if type(promise) ~= "table" or not promise.state then
  return ""
end
return "installed-status " .. tostring(promise.state)
)";
  script.close();

  const std::string threadId = "thread-installed-status-script";
  firmius::core::hooks::HookState::instance().bindThread(threadId);
  ASSERT_TRUE(firmius::core::hooks::HookState::instance().writeJson(
      firmius::core::hooks::HookState::Scope::Thread, "promise.state",
      R"("open")", "status-test"));

  auto &harness = Harness::instance();
  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.status_model_ = std::make_shared<firmius::tui::StatusBarModel>();
  state.thread_.threadId = threadId;

  state.updateStatusModel();

  ASSERT_NE(state.status_model_, nullptr);
  EXPECT_NE(std::find(state.status_model_->hook_status_lines.begin(),
                      state.status_model_->hook_status_lines.end(),
                      "installed-status open"),
            state.status_model_->hook_status_lines.end());

  std::filesystem::remove_all(tempRoot);
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

TEST(TuiStateThreadSyncTest,
     LiveStreamingEventsTriggerTranscriptRefreshAcrossSkins) {
  auto &harness = Harness::instance();
  harness.init();

  runAcrossSkins([&](firmius::tui::SkinKind skin) {
    auto &state = TuiState::instance();
    resetTuiStateForTest(state, harness);
    state.setSkinKind(skin);
    state.thread_.threadId = "thread-1";
    state.focused_agent_id_ = "agent-1";
    auto &store = firmius::tui::TUIStore::instance();
    store.thread_id = "thread-1";
    store.focused_agent_id = "agent-1";

    auto probe = ftxui::Make<TranscriptProbeComponent>();
    auto *probe_ptr = probe.get();
    state.chat_component_ = probe;

    const auto before = state.renderGeneration(firmius::tui::RefreshFlags::ChatTranscript);

    state.handleAppEvent(AgentText{"agent-1", "delta", ""});
    state.handleAppEvent(AgentProviderWaiting{"agent-1", ""});
    state.handleAppEvent(
        AgentToolCallChunk{0, "agent-1", "tool-1", "Process",
                           R"({"action":"Execute"})", ""});
    state.handleAppEvent(
        AgentToolCall{"agent-1", "tool-1", "Process",
                      R"({"action":"Execute","command":"echo hi"})", ""});
    state.handleAppEvent(
        AgentProcessSpawned{"agent-1", "proc-1", "tool-1", "echo hi", ""});
    state.handleAppEvent(
        AgentProcessOutput{"agent-1", "proc-1", "out\n", false, false, -1,
                           0.0, ""});

    const auto after =
        state.renderGeneration(firmius::tui::RefreshFlags::ChatTranscript);
    EXPECT_GT(after, before);
    EXPECT_GE(probe_ptr->transcript_changed_events, 6);
  });
}

TEST(TuiStateThreadSyncTest,
     RetryCompactionQueueAndTerminalEventsRefreshTranscriptAcrossSkins) {
  auto &harness = Harness::instance();
  harness.init();

  runAcrossSkins([&](firmius::tui::SkinKind skin) {
    auto &state = TuiState::instance();
    resetTuiStateForTest(state, harness);
    state.setSkinKind(skin);
    state.thread_.threadId = "thread-1";
    state.focused_agent_id_ = "agent-1";
    auto &store = firmius::tui::TUIStore::instance();
    store.thread_id = "thread-1";
    store.focused_agent_id = "agent-1";

    auto probe = ftxui::Make<TranscriptProbeComponent>();
    auto *probe_ptr = probe.get();
    state.chat_component_ = probe;

    const auto before = state.renderGeneration(firmius::tui::RefreshFlags::ChatTranscript);

    state.handleAppEvent(
        AgentRetrying{"agent-1", 1, 2, 429, 1000, "rate limited", "", "", ""});
    state.handleAppEvent(AgentRetryFailed{"agent-1", 429, "still limited", ""});
    state.handleAppEvent(AgentAccountSwitched{"agent-1", "acct-2", ""});
    state.handleAppEvent(AgentCompactionThinking{"agent-1", "compressing", ""});
    state.handleAppEvent(AgentCompactionText{"agent-1", "summary", ""});
    state.handleAppEvent(MessageQueued{"m1", "queued", "thread-1", "agent-1", {}});
    state.handleAppEvent(
        InternalMessageQueued{"im1", "internal", "thread-1", "agent-1"});
    state.handleAppEvent(
        AgentFinished{"agent-1", AgentOutcome{AgentOutcome::Kind::Response, "done"}, ""});
    state.handleAppEvent(AgentError{"agent-1", "oops", ""});

    const auto after =
        state.renderGeneration(firmius::tui::RefreshFlags::ChatTranscript);
    EXPECT_GT(after, before);
    EXPECT_GE(probe_ptr->transcript_changed_events, 9);
  });
}

TEST(TuiStateThreadSyncTest, SubagentSpawnDoesNotStealFocusFromParent) {
  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  AgentContext parentContext;
  parentContext.identity.name = "aster";
  parentContext.identity.role = "aster";
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
  auto &store = firmius::tui::TUIStore::instance();
  store.thread_id = threadId;
  store.focused_agent_id = "focus-parent";

  state.handleAppEvent(AgentSpawned{"focus-child", "scout", "focus-parent",
                                    "scout", "Scout", true, "", "", 0});

  EXPECT_EQ(state.focused_agent_id_, "focus-parent");
  EXPECT_EQ(store.focused_agent_id, "focus-parent");
  EXPECT_EQ(harness.focusedAgentId(), "focus-parent");
}

TEST(TuiStateThreadSyncTest,
     ProcessOutputEventTriggersTranscriptRefreshSignal) {
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

TEST(TuiStateThreadSyncTest,
     AgentInterruptedTriggersTranscriptRefreshSignal) {
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

TEST(TuiStateThreadSyncTest,
     InitAppliesPreferredPermissionModeToRequestThreads) {
  ScopedHomeOverride home(std::filesystem::temp_directory_path() /
                          "firmius_tui_thread_sync_home");

  firmius::tui::UserPreferences prefs;
  prefs.preferred_permission_mode =
      firmius::shared::ThreadPermissionMode::AlwaysAllow;
  firmius::tui::saveUserPreferences(prefs);

  auto &harness = Harness::instance();
  harness.init();

  const std::string threadId = harness.newThread({}, "/tmp", "lead");
  ASSERT_FALSE(threadId.empty());

  firmius::shared::ThreadMetadata metadata;
  for (const auto &candidate : harness.listThreads()) {
    if (candidate.threadId == threadId) {
      metadata = candidate;
      break;
    }
  }
  ASSERT_EQ(metadata.threadId, threadId);
  ASSERT_EQ(metadata.permissionMode,
            firmius::shared::ThreadPermissionMode::Request);

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.init(harness, metadata, "");

  EXPECT_EQ(state.thread_.permissionMode,
            firmius::shared::ThreadPermissionMode::AlwaysAllow);
  EXPECT_EQ(harness.currentThreadPermissionMode(),
            firmius::shared::ThreadPermissionMode::AlwaysAllow);
}


TEST(TuiStateThreadSyncTest, PermissionProfilesAreSeededAndLoadedFromPreferences) {
  ScopedHomeOverride home(std::filesystem::temp_directory_path() /
                          "firmius_tui_permission_profile_home");

  const auto permissionsDir = home.path() / ".firmius" / "permissions";
  EXPECT_FALSE(std::filesystem::exists(permissionsDir));

  firmius::tui::UserPreferences prefs;
  prefs.preferred_permission_profile = "deny";
  firmius::tui::saveUserPreferences(prefs);

  EXPECT_TRUE(std::filesystem::exists(permissionsDir / "ask.json"));
  EXPECT_TRUE(std::filesystem::exists(permissionsDir / "allow.json"));
  EXPECT_TRUE(std::filesystem::exists(permissionsDir / "deny.json"));

  const auto loaded = firmius::tui::loadUserPreferences();
  ASSERT_TRUE(loaded.preferred_permission_profile.has_value());
  EXPECT_EQ(*loaded.preferred_permission_profile, "deny");
  ASSERT_TRUE(loaded.preferred_permission_mode.has_value());
  EXPECT_EQ(*loaded.preferred_permission_mode,
            firmius::shared::ThreadPermissionMode::DenyAll);
}
TEST(TuiStateThreadSyncTest, SetSkinKindPersistsSelectedSkinPreference) {
  ScopedHomeOverride home(std::filesystem::temp_directory_path() /
                          "firmius_tui_skin_pref_home");

  auto &harness = Harness::instance();
  harness.init();

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.setSkinKind(firmius::tui::SkinKind::Claudex);

  const auto prefs = firmius::tui::loadUserPreferences();
  ASSERT_TRUE(prefs.skin_kind.has_value());
  EXPECT_EQ(*prefs.skin_kind, firmius::tui::SkinKind::Claudex);
}

TEST(TuiStateThreadSyncTest, DeferredUiMutationsDrainOnEventLoopTick) {
  auto &harness = Harness::instance();
  harness.init();

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);

  int mutationCount = 0;
  state.deferUiMutation([&mutationCount]() { ++mutationCount; });

  EXPECT_EQ(mutationCount, 0);
  state.drainEvents();
  EXPECT_EQ(mutationCount, 1);
}

TEST(TuiStateThreadSyncTest, WelcomePromptSubmitCreatesThreadAndTransitionsToChat) {
  ScopedHomeOverride home(std::filesystem::temp_directory_path() /
                          "firmius_tui_welcome_submit_home");
  auto &harness = Harness::instance();
  harness.init();

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.initModels();
  state.setViewMode(TuiState::ViewMode::Welcome);
  firmius::tui::TUIStore::instance().view_mode =
      firmius::tui::TUIStore::ViewMode::Welcome;
  firmius::tui::TUIStore::instance().thread_id.clear();
  firmius::tui::TUIStore::instance().focused_agent_id.clear();
  state.thread_ = {};
  state.focused_agent_id_.clear();

  state.submitPrompt("hai", {});

  bool transitioned = false;
  for (int i = 0; i < 80; ++i) {
    state.drainEvents();
    if (state.getViewMode() == TuiState::ViewMode::Chat &&
        !state.thread_.threadId.empty() && !state.focused_agent_id_.empty()) {
      transitioned = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  ASSERT_TRUE(transitioned);
  EXPECT_EQ(firmius::tui::TUIStore::instance().thread_id,
            state.thread_.threadId);
  EXPECT_EQ(firmius::tui::TUIStore::instance().focused_agent_id,
            state.focused_agent_id_);
}

TEST(TuiStateThreadSyncTest,
     WelcomePromptSubmitOptimisticallyLeavesWelcomeAndShowsUserMessageImmediately) {
  ScopedHomeOverride home(std::filesystem::temp_directory_path() /
                          "firmius_tui_welcome_submit_optimistic_home");
  auto &harness = Harness::instance();
  harness.init();

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);
  state.initModels();
  state.setViewMode(TuiState::ViewMode::Welcome);
  state.thread_ = {};
  state.focused_agent_id_.clear();

  state.submitPrompt("hai", {});

  EXPECT_EQ(state.getViewMode(), TuiState::ViewMode::Chat);
  ASSERT_TRUE(state.history_);
  ASSERT_EQ(state.history_->turns.size(), 1u);
  ASSERT_EQ(state.history_->turns.front().messages.size(), 1u);
  const auto &msg = state.history_->turns.front().messages.front();
  ASSERT_EQ(msg.role, firmius::shared::Role::User);
  ASSERT_FALSE(msg.content.empty());
  const auto *text =
      std::get_if<firmius::shared::TextContent>(&msg.content.front());
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(text->text, "hai");
}

TEST(TuiStateThreadSyncTest,
     WelcomeBootstrapThenLeadSpawnThenStreamTextRefreshesTranscript) {
  auto &harness = Harness::instance();
  harness.init();

  runAcrossSkins([&](firmius::tui::SkinKind skin) {
    auto &state = TuiState::instance();
    resetTuiStateForTest(state, harness);
    state.setSkinKind(skin);

    auto probe = ftxui::Make<TranscriptProbeComponent>();
    auto *probe_ptr = probe.get();
    state.chat_component_ = probe;

    // optimistic user submit already moved us into Chat with local history
    state.submitPrompt("hai", {});
    ASSERT_EQ(state.getViewMode(), TuiState::ViewMode::Chat);
    ASSERT_TRUE(state.history_);
    ASSERT_FALSE(state.history_->turns.empty());
    const int after_submit = probe_ptr->transcript_changed_events;
    EXPECT_GE(after_submit, 1);

    // provisional thread open arrives before focused lead is known
    firmius::shared::ThreadMetadata metadata;
    metadata.threadId = "thread-1";
    metadata.title = "New Thread";
    state.applyThreadOpened(metadata, "", true);
    state.applyPendingRefreshes();
    EXPECT_EQ(state.thread_.threadId, "thread-1");
    EXPECT_TRUE(state.focused_agent_id_.empty() ||
                state.focused_agent_id_ == "lead-1");
    const int after_thread_open = probe_ptr->transcript_changed_events;
    EXPECT_GE(after_thread_open, after_submit + 1);

    firmius::tui::TUIStore::instance().focused_agent_id.clear();

    // lead spawn should focus the agent (or at least ensure subsequent
    // streaming text refreshes the transcript once the UI is focused on it)
    AgentContext leadContext;
    leadContext.identity.name = "aster";
    leadContext.identity.role = "aster";
    leadContext.history = std::make_shared<firmius::shared::AgentHistory>();
    leadContext.history->threadId = "thread-1";
    ScopedRegisteredAgent lead("lead-1", leadContext);

    // Simulate UI focus landing on the new lead agent.
    state.focused_agent_id_ = "lead-1";
    firmius::tui::TUIStore::instance().focused_agent_id = "lead-1";

    const auto before_stream_render =
        state.renderGeneration(firmius::tui::RefreshFlags::ChatTranscript);
    const int before_stream_events = probe_ptr->transcript_changed_events;
    state.handleAppEvent(AgentText{"lead-1", "streaming reply", ""});
    const auto after_stream_render =
        state.renderGeneration(firmius::tui::RefreshFlags::ChatTranscript);

    EXPECT_GT(after_stream_render, before_stream_render);
    EXPECT_GE(probe_ptr->transcript_changed_events, before_stream_events + 1);
  });
}

TEST(TuiStateThreadSyncTest,
     ProvisionalWelcomeThreadOpenDoesNotClearAlreadyEstablishedLeadFocus) {
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
  leadContext.identity.name = "aster";
  leadContext.identity.role = "aster";
  leadContext.history = std::make_shared<firmius::shared::AgentHistory>();
  leadContext.history->threadId = "thread-1";
  ScopedRegisteredAgent lead("lead-1", leadContext);
  ASSERT_TRUE(harness.setFocusedAgent("lead-1"));

  state.focused_agent_id_ = "lead-1";
  firmius::tui::TUIStore::instance().focused_agent_id = "lead-1";

  state.applyThreadOpened(metadata, "", true);

  EXPECT_EQ(state.focused_agent_id_, "lead-1");
  EXPECT_EQ(firmius::tui::TUIStore::instance().focused_agent_id, "lead-1");
}

TEST(TuiStateThreadSyncTest, FileReferenceAutocompleteIndexesInBackground) {
  auto &harness = Harness::instance();
  harness.init();

  auto &state = TuiState::instance();
  resetTuiStateForTest(state, harness);

  const auto root = std::filesystem::temp_directory_path() /
                    "firmius_tui_file_reference_async";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "src");
  {
    std::ofstream out(root / "src" / "needle.txt");
    out << "needle";
  }

  auto &controller = firmius::tui::InputController::instance();
  {
    std::lock_guard<std::mutex> lock(controller.file_reference_cache_mutex_);
    controller.file_reference_cache_ready_ = false;
    controller.file_reference_cache_loading_ = false;
    controller.file_reference_cache_root_.clear();
    controller.file_reference_cache_paths_.clear();
  }

  const auto first =
      controller.completeFileReferences("needle", root.string());
  EXPECT_TRUE(first.empty());

  for (int attempt = 0; attempt < 100; ++attempt) {
    state.drainEvents();
    {
      std::lock_guard<std::mutex> lock(controller.file_reference_cache_mutex_);
      if (controller.file_reference_cache_ready_)
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  state.drainEvents();

  const auto second =
      controller.completeFileReferences("needle", root.string());
  EXPECT_FALSE(second.empty());

  std::filesystem::remove_all(root);
}

} // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  std::_Exit(result == 0 ? 0 : 1);
}
