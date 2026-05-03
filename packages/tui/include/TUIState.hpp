#ifndef FIRMIUS_TUI_STATE_HPP
#define FIRMIUS_TUI_STATE_HPP

#include "ActivePlanState.hpp"
#include "EventQueue.hpp"
#include "Events.hpp"
#include "NotificationManager.hpp"
#include "SkinConfig.hpp"
#include "StreamStateManager.hpp"
#include "WorkPanelLayout.hpp"
#include "components/AgentStrip.hpp"
#include "components/ContextLane.hpp"
#include "components/PlanLane.hpp"
#include "components/TodoLane.hpp"
#include "controllers/UiActions.hpp"
#include "models/PermissionModel.hpp"
#include "models/TUIStore.hpp"
#include "models/TranscriptModel.hpp"
#include "views/MainView.hpp"
#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/box.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace firmius::core {
class Harness;
struct CompactionSnapshot;
}

namespace firmius::tui {

enum class RefreshFlags : unsigned int {
  None = 0,
  Status = 1u << 0,
  AgentStrip = 1u << 1,
  PlanLane = 1u << 2,
  TodoLane = 1u << 3,
  ContextLane = 1u << 4,
  // Render-only surfaces (do not necessarily imply model refresh).
  InputBar = 1u << 6,
  Welcome = 1u << 7,
  LiveStatusRow = 1u << 8,
  WorkPanel = 1u << 9,
  ChatTranscript = 1u << 5,
};

constexpr inline RefreshFlags operator|(RefreshFlags a, RefreshFlags b) {
  return static_cast<RefreshFlags>(static_cast<unsigned int>(a) |
                                  static_cast<unsigned int>(b));
}
constexpr inline RefreshFlags operator&(RefreshFlags a, RefreshFlags b) {
  return static_cast<RefreshFlags>(static_cast<unsigned int>(a) &
                                  static_cast<unsigned int>(b));
}

struct TuiProfilingStats {
  std::atomic<uint64_t> app_event_enqueued{0};
  std::atomic<uint64_t> custom_event_posted{0};
  std::atomic<uint64_t> custom_event_drained{0};
  std::atomic<uint64_t> on_event_dispatch_count{0};
  std::atomic<int64_t> on_event_dispatch_ns{0};
  std::atomic<uint64_t> thread_changed_count{0};
  std::atomic<int64_t> thread_changed_ns{0};
  std::atomic<uint64_t> rebuild_tool_calls_count{0};
  std::atomic<int64_t> rebuild_tool_calls_ns{0};
  std::atomic<uint64_t> chat_window_rebuild_count{0};
  std::atomic<int64_t> chat_window_rebuild_ns{0};
  std::atomic<uint64_t> frame_render_count{0};
  std::atomic<int64_t> frame_render_ns{0};
};

TuiProfilingStats &tuiProfilingStats();
bool isTuiStartupProfilingEnabled();
void noteTuiAppEventEnqueued();
void noteTuiCustomEventPosted();
void noteTuiCustomEventDrained();
void noteTuiOnEventDispatch(std::chrono::nanoseconds elapsed);
void noteTuiThreadChanged(std::chrono::nanoseconds elapsed);
void noteTuiRebuildToolCalls(std::chrono::nanoseconds elapsed);
void noteTuiChatWindowRebuild(std::chrono::nanoseconds elapsed);
void noteTuiFrameRendered(std::chrono::nanoseconds elapsed);
std::string tuiProfilingSummaryText();

namespace detail {
bool shouldNotifyHiddenChatError(const std::string &focused_id,
                                 const std::string &error_id, bool hide_errors);
}

std::size_t BuildFocusedChatLiveMeasurementSignature(
    const StreamStateManager &stream_state, const std::string &focused_agent_id,
    const std::string &thread_id,
    const std::unordered_set<std::string> &persisted_tool_call_ids);

std::vector<shared::AgentTurn> expandCompactionTranscriptForDisplay(
    const std::vector<shared::AgentTurn> &turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        &snapshots);

class TuiState {
  friend class AppController;
  friend class MainView;

public:
  static TuiState &instance();

  TuiState(const TuiState &) = delete;
  TuiState &operator=(const TuiState &) = delete;

  void init(firmius::core::Harness &harness,
            const shared::ThreadMetadata &thread,
            const std::string &focused_agent_id);
  void initModels();
  void attachScreen(ftxui::ScreenInteractive *screen);
  // Request a temporary fixed-interval UI tick. This is used to animate small
  // widgets (spinners/welcome/live row) without forcing a global always-on
  // ticker. Each request extends the tick window.
  void requestAnimationTick(std::chrono::milliseconds interval,
                            std::chrono::milliseconds ttl =
                                std::chrono::milliseconds(1500));
  void shutdown();

  InputBarModel &getInputBarModel();
  void clearInputBuffer();
  bool handleCtrlC();
  void requestQuit();

  void triggerTranscriptUndoFromHotkey();
  void triggerTranscriptRedoFromHotkey();
  void triggerEditUndoFromHotkey();
  void triggerTranscriptUndoToUserBoundaryFromHotkey();
  void triggerEditRedoFromHotkey();

  bool isQuitArmed() const;
  std::string exitSummaryText() const;

  ftxui::Component root();

  enum class ViewMode { Welcome, Chat, ProcessFocus };
  void setViewMode(ViewMode mode);
  ViewMode getViewMode() const;

  void openModal(const std::string &name);
  void openModalDirect(ftxui::Component modal,
                       const std::string &modal_name = "");
  void popModal();
  void popModalImmediate();
  void replaceModalDirect(ftxui::Component modal);
  void clearModals();
  void deferUiMutation(std::function<void()> action);
  void runBackgroundTask(std::function<void()> action);
  void postAction(UiAction action);
  void submitPrompt(std::string text,
                    std::vector<firmius::shared::ImageContent> images = {});
  void requestThreadOpen(std::optional<std::string> thread_id,
                         bool resume_last = false,
                         std::string loading_message = "",
                         std::string loading_detail = "");
  void applyThreadOpened(const shared::ThreadMetadata &metadata,
                         const std::string &focused_agent_id,
                         bool preserve_live_state);
  void setLoadingMessage(std::string message);
  void clearLoadingMessage();
  std::string loadingMessage() const;
  void setLoadingProgress(float progress);
  void clearLoadingProgress();
  float loadingProgress() const;
  void setLoadingDetail(std::string detail);
  void clearLoadingDetail();
  std::string loadingDetail() const;
  void clearLoadingState();

  void postEvent(ftxui::Event event);
  bool cycleThreadPermissionMode();
  bool hasActiveThread() const;
  std::string currentThreadId() const;
  shared::ThreadPermissionMode currentThreadPermissionMode() const;
  bool needsAnimationTick() const;
  bool focusAgent(const std::string &agent_id);

  SkinKind currentSkinKind() const;
  void setSkinKind(SkinKind kind);
  void applySkinConfig(const SkinConfig &config);
  const SkinConfig &skinConfig() const;

  void loadUserPreferences();
  void persistUserPreferences() const;
  void syncCurrentThreadMetadataFromHarness(bool preserve_live_state);
  // Regression fix: Need to track these for redo actions
  std::optional<shared::TranscriptUndoAction> last_transcript_undo_action_;
  std::optional<shared::TranscriptRedoAction> last_transcript_redo_action_;
  std::optional<shared::EditUndoAction> last_edit_undo_action_;
  std::optional<shared::EditRedoAction> last_edit_redo_action_;

  bool suppress_next_history_undone_refresh_ = false;

  void refreshFocusedHistory();
  void notifyChatTranscriptChanged();

  // Render invalidation: lets individual surfaces opt-in to re-rendering on the
  // next Draw() without forcing expensive sibling surfaces to re-render.
  void requestRender(RefreshFlags flags);
  uint64_t renderGeneration(RefreshFlags flag) const;

  // Kept public for backwards unit test compatibility (#define private public)
  firmius::core::Harness *harness_ = nullptr;
  shared::ThreadMetadata thread_;
  std::string focused_agent_id_;
  std::shared_ptr<shared::AgentHistory> history_;
  StreamStateManager stream_state_;

  std::shared_ptr<TitleBarModel> title_model_;
  std::shared_ptr<StatusBarModel> status_model_;
  std::shared_ptr<InputBarModel> input_model_;
  std::shared_ptr<AgentStripModel> agent_strip_model_;
  std::shared_ptr<PlanLaneModel> plan_lane_model_;
  std::shared_ptr<TodoLaneModel> todo_lane_model_;
  std::shared_ptr<ContextLaneModel> context_lane_model_;

  ActivePlanState active_plan_state_;

  ViewMode view_mode_ = ViewMode::Chat;
  unsigned int pending_refresh_flags_ = 0;
  unsigned int pending_render_flags_ = 0;
  uint64_t render_generation_ = 0;
  std::array<uint64_t, 16> render_surface_generation_{};
  ftxui::Component chat_component_;
  ftxui::Component root_component_;

  std::vector<ftxui::Component> modals_;
  ftxui::Component modal_container_;
  std::vector<std::function<void()>> deferred_ui_mutations_;

  ftxui::ScreenInteractive *screen_ = nullptr;
  firmius::shared::EventQueue<UiAction> ui_action_queue_;
  int subscription_id_ = -1;
  void drainEvents();
  std::atomic<bool> custom_event_pending_ = false;
  std::jthread animation_tick_thread_;

  std::mutex animation_tick_mutex_;
  std::chrono::steady_clock::time_point animation_tick_until_{};
  std::chrono::milliseconds animation_tick_interval_{0};
  uint64_t animation_tick_generation_ = 0;
  std::unordered_map<std::string, std::shared_ptr<shared::AgentHistory>>
      agent_history_cache_;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      agent_persisted_tool_call_ids_cache_;

  std::shared_ptr<MainView> main_view_;

  // Claudex Soul State
  std::string live_row_current_phrase_;
  std::string live_row_previous_phrase_;
  std::string live_row_last_phrase_key_;
  std::string live_row_pending_phrase_;
  std::string live_row_pending_phrase_key_;
  uint64_t live_row_phrase_rng_state_ = 0xDECAFBADULL;
  std::chrono::steady_clock::time_point
      live_row_phrase_transition_started_at_{};
  std::chrono::steady_clock::time_point live_row_phrase_next_switch_at_{};
  std::chrono::steady_clock::time_point live_row_phrase_min_visible_until_{};
  std::chrono::steady_clock::time_point last_live_raf_request_{};
  std::unordered_map<std::string, uint64_t> agent_work_start_ms_;
  std::vector<std::string> bottom_hook_status_lines_;

  std::optional<std::chrono::steady_clock::time_point> quit_arm_deadline_;
  std::size_t quit_arm_generation_ = 0;
  std::jthread quit_arm_thread_;
  std::string loading_message_;
  std::optional<std::chrono::steady_clock::time_point> loading_auto_clear_at_;
  mutable std::mutex loading_message_mutex_;
  std::mutex deferred_ui_mutations_mutex_;
  float loading_progress_ = -1.0f;
  std::vector<std::jthread> background_ui_tasks_;
  std::string loading_detail_;
  std::mutex background_ui_tasks_mutex_;
  mutable std::mutex loading_progress_mutex_;
  mutable std::mutex loading_detail_mutex_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      quota_refresh_last_started_;
  std::unordered_set<std::string> quota_refresh_inflight_;
  std::mutex quota_refresh_mutex_;

private:
  TuiState();
  ~TuiState() = default;

  void onEvent(const shared::AppEvent &ev);
  void dispatchAction(const UiAction &action);
  void expireLoadingStateIfNeeded();
  void requestRefresh(RefreshFlags flags);
  void applyPendingRenders();
  void applyPendingRefreshes();
  void updateStatusModel();
  void updateAgentStripModel();
  void updatePlanLaneModel();
  void updateContextLaneModel();
  void updateTodoLaneModel();
  void scheduleQuotaRefresh(const std::string &providerId);
  void rebuildEditableUserMessages();
  void drainDeferredUiMutations();
  bool isEditModeSelection(uint64_t timestamp) const;
  void selectEditableMessageByTimestamp(uint64_t timestamp);
  bool commitSelectedEditableMessageToInput();

public:
  SkinConfig skin_config_ = defaultSkinConfig(SkinKind::Firmius);
  void handleAppEvent(const shared::AppEvent &ev);
};

void noteTuiModalOpenRequested(const std::string &name);
std::vector<std::string>
focusCycleCandidates(const std::string &focusedAgentId);

} // namespace firmius::tui

#endif
