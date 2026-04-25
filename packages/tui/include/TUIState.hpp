#ifndef FIRMIUS_TUI_STATE_HPP
#define FIRMIUS_TUI_STATE_HPP

#include "Context.hpp"
#include "EventQueue.hpp"
#include "Events.hpp"
#include "ActivePlanState.hpp"
#include "StreamStateManager.hpp"
#include "NotificationManager.hpp"
#include "WorkPanelLayout.hpp"
#include "persistence/ThreadManager.hpp"
#include "SkinConfig.hpp"
#include "components/ContextLane.hpp"
#include "components/TodoLane.hpp"
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/box.hpp>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <chrono>
namespace firmius::core {
class Harness;
}

namespace firmius::tui {

struct TitleBarModel;
struct StatusBarModel;
struct InputBarModel;
struct AgentStripModel;
struct PlanLaneModel;
struct TodoLaneModel;
struct ContextLaneModel;


template <typename T>
inline void HashCombineForFocusedChatMeasurement(std::size_t &seed,
                                                const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}
inline std::size_t BuildFocusedChatLiveMeasurementSignature(
    const firmius::tui::StreamStateManager &stream_state,
    const std::string &focused_agent_id, const std::string &thread_id,
    const std::unordered_set<std::string> &persisted_tool_call_ids) {
  std::size_t signature = 0;
  HashCombineForFocusedChatMeasurement(signature, focused_agent_id);
  HashCombineForFocusedChatMeasurement(signature, persisted_tool_call_ids.size());
  HashCombineForFocusedChatMeasurement(signature, thread_id);

  auto bucket = [](std::size_t size) { return size / 512; };

  if (const auto *s = stream_state.getStream(focused_agent_id)) {
    HashCombineForFocusedChatMeasurement(signature, bucket(s->thinking.size()));
    HashCombineForFocusedChatMeasurement(signature, bucket(s->text.size()));
    HashCombineForFocusedChatMeasurement(signature,
                                        bucket(s->compaction_thinking.size()));
    HashCombineForFocusedChatMeasurement(signature,
                                        bucket(s->compaction_text.size()));
    HashCombineForFocusedChatMeasurement(signature,
                                        bucket(s->compaction_completion.size()));
    HashCombineForFocusedChatMeasurement(signature, s->compaction_active);
    HashCombineForFocusedChatMeasurement(signature, s->compaction_finished);
    HashCombineForFocusedChatMeasurement(signature, s->provider_waiting);
  }

  std::size_t focused_timeline = 0;
  for (const auto &entry : stream_state.getTimeline()) {
    if (entry.agentId == focused_agent_id) {
      focused_timeline++;
      HashCombineForFocusedChatMeasurement(signature,
                                          static_cast<int>(entry.kind));
    }
  }
  HashCombineForFocusedChatMeasurement(signature, focused_timeline);

  std::size_t focused_tool_calls = 0;
  for (const auto &[id, view] : stream_state.getToolCalls()) {
    (void)id;
    if (view && view->agentId == focused_agent_id) {
      focused_tool_calls++;
      HashCombineForFocusedChatMeasurement(signature,
                                          static_cast<int>(view->phase));
      HashCombineForFocusedChatMeasurement(signature, view->success);
    }
  }
  HashCombineForFocusedChatMeasurement(signature, focused_tool_calls);

  const auto bucketQueued = [](std::size_t size) { return size / 128; };
  std::size_t queued_count = 0;
  for (const auto &entry : stream_state.getQueuedMessages()) {
    if ((!entry.agent_id.empty() && entry.agent_id != focused_agent_id) ||
        (!entry.thread_id.empty() && entry.thread_id != thread_id)) {
      continue;
    }
    queued_count++;
    HashCombineForFocusedChatMeasurement(signature,
                                        bucketQueued(entry.text.size()));
    HashCombineForFocusedChatMeasurement(signature, entry.image_count);
  }
  HashCombineForFocusedChatMeasurement(signature, queued_count);

  std::size_t queued_internal_count = 0;
  for (const auto &entry : stream_state.getQueuedInternalMessages()) {
    if ((!entry.agent_id.empty() && entry.agent_id != focused_agent_id) ||
        (!entry.thread_id.empty() && entry.thread_id != thread_id)) {
      continue;
    }
    queued_internal_count++;
    HashCombineForFocusedChatMeasurement(signature,
                                        bucketQueued(entry.text.size()));
  }
  HashCombineForFocusedChatMeasurement(signature, queued_internal_count);
  return signature;
}


bool isTuiStartupProfilingEnabled();
void noteTuiCustomEventPosted();
void noteTuiCustomEventDrained();
void noteTuiOnEventDispatch(std::chrono::nanoseconds elapsed);
void noteTuiThreadChanged(std::chrono::nanoseconds elapsed);
void noteTuiRebuildToolCalls(std::chrono::nanoseconds elapsed);
void noteTuiChatWindowRebuild(std::chrono::nanoseconds elapsed);
void noteTuiFrameRendered(std::chrono::nanoseconds elapsed);
void noteTuiModalOpenRequested(const std::string &name);
void noteTuiModalFirstPaint(const std::string &name);
void noteTuiRequestAnimationFrameFromState();
void noteTuiRequestAnimationFrameFromScrollableBoxWidthChange();
void noteTuiRequestAnimationFrameFromAgentStripSpinner();
void noteTuiRequestAnimationFrameFromGlintEffect();
std::string tuiProfilingSummaryText();

namespace detail {

inline std::optional<std::string> compactionIdFromTurnIdForDisplay(
    const std::string& turnId) {
  constexpr const char* prefixes[] = {"compaction-start-", "compaction-summary-",
                                      "compaction-end-"};
  for (const char* prefix : prefixes) {
    const std::size_t len = std::char_traits<char>::length(prefix);
    if (turnId.rfind(prefix, 0) == 0) {
      return turnId.substr(len);
    }
  }
  return std::nullopt;
}

inline std::size_t overlappingSnapshotSuffixLengthForDisplay(
    const std::vector<shared::AgentTurn>& snapshotTurns,
    const std::vector<shared::AgentTurn>& currentTurns,
    std::size_t currentStart) {
  const std::size_t maxCount =
      std::min(snapshotTurns.size(), currentTurns.size() - currentStart);
  for (std::size_t count = maxCount; count > 0; --count) {
    bool allMatch = true;
    for (std::size_t i = 0; i < count; ++i) {
      if (snapshotTurns[snapshotTurns.size() - count + i].turnId !=
          currentTurns[currentStart + i].turnId) {
        allMatch = false;
        break;
      }
    }
    if (allMatch) {
      return count;
    }
  }
  return 0;
}

inline std::size_t overlappingRenderedPrefixLengthForDisplay(
    const std::vector<shared::AgentTurn>& renderedTurns,
    const std::vector<shared::AgentTurn>& snapshotTurns) {
  const std::size_t maxCount =
      std::min(renderedTurns.size(), snapshotTurns.size());
  for (std::size_t count = maxCount; count > 0; --count) {
    bool allMatch = true;
    for (std::size_t i = 0; i < count; ++i) {
      if (renderedTurns[renderedTurns.size() - count + i].turnId !=
          snapshotTurns[i].turnId) {
        allMatch = false;
        break;
      }
    }
    if (allMatch) {
      return count;
    }
  }
  return 0;
}

inline std::vector<shared::AgentTurn> expandCompactionTranscriptTurnsForDisplay(
    const std::vector<shared::AgentTurn>& turns,
    const std::unordered_map<std::string, core::CompactionSnapshot>& snapshots,
    std::unordered_set<std::string>& expanded_ids) {
  std::vector<shared::AgentTurn> result;
  for (std::size_t i = 0; i < turns.size(); ++i) {
    const auto compactionId = compactionIdFromTurnIdForDisplay(turns[i].turnId);
    if (!compactionId.has_value() ||
        turns[i].turnId.rfind("compaction-start-", 0) != 0) {
      result.push_back(turns[i]);
      continue;
    }

    std::size_t blockEnd = i;
    while (blockEnd + 1 < turns.size()) {
      const auto nextId =
          compactionIdFromTurnIdForDisplay(turns[blockEnd + 1].turnId);
      if (!nextId.has_value() || *nextId != *compactionId) {
        break;
      }
      ++blockEnd;
      if (turns[blockEnd].turnId.rfind("compaction-end-", 0) == 0) {
        break;
      }
    }

    auto snapshotIt = snapshots.find(*compactionId);
    if (snapshotIt != snapshots.end() && !expanded_ids.count(*compactionId)) {
      expanded_ids.insert(*compactionId);
      const auto& snapshotTurns = snapshotIt->second.turns;
      auto expandedSnapshot = expandCompactionTranscriptTurnsForDisplay(
          snapshotTurns, snapshots, expanded_ids);
      const std::size_t renderedOverlap =
          overlappingRenderedPrefixLengthForDisplay(result, expandedSnapshot);
      if (renderedOverlap > 0 && renderedOverlap <= expandedSnapshot.size()) {
        expandedSnapshot.erase(expandedSnapshot.begin(),
                               expandedSnapshot.begin() + renderedOverlap);
      }
      result.insert(result.end(), expandedSnapshot.begin(),
                    expandedSnapshot.end());

      for (std::size_t j = i; j <= blockEnd; ++j) {
        result.push_back(turns[j]);
      }

      const std::size_t overlap = overlappingSnapshotSuffixLengthForDisplay(
          snapshotTurns, turns, blockEnd + 1);
      const std::size_t nextIndex = blockEnd + overlap + 1;
      if (nextIndex >= turns.size()) {
        break;
      }
      i = nextIndex - 1;
      continue;
    }

    for (std::size_t j = i; j <= blockEnd; ++j) {
      result.push_back(turns[j]);
    }
    i = blockEnd;
  }
  return result;
}

} // namespace detail

inline std::vector<shared::AgentTurn> expandCompactionTranscriptForDisplay(
    const std::vector<shared::AgentTurn>& turns,
    const std::unordered_map<std::string, core::CompactionSnapshot>& snapshots) {
  std::unordered_set<std::string> expanded_ids;
  return detail::expandCompactionTranscriptTurnsForDisplay(turns, snapshots,
                                                           expanded_ids);
}

namespace detail {

inline bool shouldNotifyHiddenChatError(const std::string& focused_agent_id,
                                        const std::string& error_agent_id,
                                        bool hide_errors) {
  return hide_errors && !focused_agent_id.empty() &&
         error_agent_id == focused_agent_id;
}

} // namespace detail

class TuiState {
public:
  static TuiState &instance();

  TuiState(const TuiState &) = delete;
  TuiState &operator=(const TuiState &) = delete;

  TuiState(TuiState &&) = delete;
  TuiState &operator=(TuiState &&) = delete;

  void init(firmius::core::Harness &harness,
            const shared::ThreadMetadata &thread,
            const std::string &focused_agent_id);
  void attachScreen(ftxui::ScreenInteractive *screen);
  void shutdown();

  InputBarModel& getInputBarModel();
  void clearInputBuffer();
  bool handleCtrlC();
  void requestQuit();
  bool isQuitArmed() const;
  std::string exitSummaryText() const;

  std::string getProcessOutput(const std::string &pid);
  std::string getSubagentOutput(const std::string &subagentId);

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

private:
  TuiState();
  ~TuiState() = default;

  void loadUserPreferences();
  void persistUserPreferences() const;
  void activatePermissionRequest(
      const shared::PermissionEscalationRequest &request);
  void clearActivePermissionRequest();
  void promoteNextPermissionRequest();
  enum class RefreshFlags : unsigned int {
    None = 0,
    Status = 1u << 0,
    AgentStrip = 1u << 1,
    PlanLane = 1u << 2,
    TodoLane = 1u << 3,
    ContextLane = 1u << 4,
    ChatTranscript = 1u << 5,
  };

  void onEvent(const shared::AppEvent &ev);
  void requestRefresh(RefreshFlags flags);
  void applyPendingRefreshes();
  void notifyChatTranscriptChanged();
  std::string statusText() const;
  void updateStatusModel();
  void updateAgentStripModel();
  void updatePlanLaneModel();
  void updateTodoLaneModel();
  void updateContextLaneModel();
  void syncCurrentThreadMetadataFromHarness(bool preserve_live_state);
  void refreshFocusedHistory();
  void rebuildEditableUserMessages();
  bool isEditModeSelection(uint64_t timestamp) const;
  void selectEditableMessageByTimestamp(uint64_t timestamp);
  bool commitSelectedEditableMessageToInput();
  void dismissUndoRedoNotices();
  void syncUndoRedoNoticeHistory();
  void setTranscriptUndoNotice(const shared::TranscriptUndoAction &action,
                               int turnsUndone);
  void setEditUndoNotice(const shared::EditUndoAction &action,
                         std::vector<std::string> paths);
  void triggerTranscriptUndoFromHotkey();
  void triggerTranscriptRedoFromHotkey();
  void triggerEditUndoFromHotkey();
  void triggerEditRedoFromHotkey();
  std::optional<shared::EditBatchSummary> latestFocusedEditBatch() const;
  std::optional<shared::EditBatchSummary> latestFocusedUndoneEditBatch() const;
  std::vector<std::string> editPathsForBatch(const std::string &editBatchId) const;
  void updateFocusedEditFollowUpNotice();
  std::optional<shared::Plan> loadActivePlanForThread(
      const shared::ThreadMetadata &thread) const;
  const shared::WorkChunk *
  findExecutorChunk(const std::optional<shared::Plan> &plan) const;
  std::string findExecutorChunkTitle(const std::optional<shared::Plan> &plan) const;

  firmius::core::Harness *harness_ = nullptr;
  shared::ThreadMetadata thread_;
  std::string focused_agent_id_;
  std::shared_ptr<shared::AgentHistory> history_;
  int subscription_id_ = -1;
  ftxui::ScreenInteractive *screen_ = nullptr;

  firmius::shared::EventQueue<shared::AppEvent> event_queue_;
  void drainEvents();
  unsigned int pending_refresh_flags_ = 0;
  bool custom_event_pending_ = false;

  StreamStateManager stream_state_;

  std::string input_;
  int cursor_ = 0;

  std::shared_ptr<TitleBarModel> title_model_;
  std::shared_ptr<StatusBarModel> status_model_;
  std::shared_ptr<InputBarModel> input_model_;
  std::shared_ptr<AgentStripModel> agent_strip_model_;
  std::shared_ptr<PlanLaneModel> plan_lane_model_;
  std::shared_ptr<TodoLaneModel> todo_lane_model_;
  std::shared_ptr<ContextLaneModel> context_lane_model_;
  ActivePlanState active_plan_state_;

  ViewMode view_mode_ = ViewMode::Chat;
  std::vector<ftxui::Component> modals_; // Used as a stack
  std::vector<std::function<void()>> deferred_ui_mutations_;
  bool pending_modal_clear_ =
      false; // Deferred clear to avoid UB in modal handlers
  bool diffs_expanded_ = true; // Ctrl+G toggle for diff expansion
  WorkPanelTab selected_work_panel_tab_ = WorkPanelTab::Context;
  bool show_agent_strip_ = true;
  bool show_work_panel_ = true;
  int agent_strip_visible_rows_ = 4;
  int work_panel_height_override_ = 0;
  bool edit_mode_active_ = false;
  struct EditableUserMessage {
    uint64_t timestamp = 0;
    std::string text;
    std::vector<shared::ImageContent> images;
  };
  std::optional<EditableUserMessage> pending_edit_message_;
  std::vector<EditableUserMessage> editable_user_messages_;
  int selected_editable_message_index_ = -1;
  bool suppress_next_history_undone_refresh_ = false;

  ftxui::Component root_component_;
  struct UndoRedoNoticeState {
    std::string transcriptUndoActionId;
    int transcriptTurnsUndone = 0;
    bool transcriptRedoAvailable = false;
    std::string editUndoActionId;
    std::vector<std::string> editPaths;
    bool editRedoAvailable = false;
    std::uint64_t revision = 0;
  };
  UndoRedoNoticeState undo_redo_notice_state_;
  std::shared_ptr<shared::AgentHistory> notice_history_overlay_;
  static constexpr const char *kUndoNoticeTurnId = "system-note-undo-redo";
  static constexpr const char *kEditNoticeTurnId = "system-note-edit-undo-redo";
  std::optional<shared::TranscriptUndoAction> last_transcript_undo_action_;
  std::optional<shared::TranscriptRedoAction> last_transcript_redo_action_;
  std::optional<shared::EditUndoAction> last_edit_undo_action_;
  std::optional<std::string> pending_edit_redo_batch_id_;
  std::optional<std::string> pending_edit_redo_notice_text_;
  std::uint64_t undo_redo_notice_revision_ = 0;


  ftxui::Component chat_component_;
  ftxui::Component input_component_;
  std::unordered_map<std::string, uint64_t> agent_work_start_ms_;
  std::unordered_map<std::string, std::shared_ptr<shared::AgentHistory>>
      agent_history_cache_;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      agent_persisted_tool_call_ids_cache_;
  std::filesystem::path file_reference_cache_root_;
  std::vector<std::string> file_reference_cache_paths_;
  bool file_reference_cache_ready_ = false;
  std::optional<shared::PermissionEscalationRequest> pending_permission_request_;
  std::vector<shared::PermissionEscalationRequest> pending_permission_queue_;
  std::vector<shared::PermissionResponse> pending_permission_responses_;
  std::vector<std::string> pending_permission_labels_;
  std::vector<ftxui::Box> pending_permission_option_boxes_;
  int pending_permission_selected_ = 0;
  
  // Process focus mode
  std::string focused_process_id_;
  bool process_focus_expanded_ = false;
  int last_terminal_width_ = 0;
  int last_terminal_height_ = 0;
  ftxui::Box work_panel_separator_box_;
  ftxui::Box agent_strip_separator_box_;
  ftxui::CapturedMouse active_drag_mouse_;
  enum class DragTarget { None, WorkPanel, AgentStrip };
  DragTarget active_drag_target_ = DragTarget::None;
  int drag_origin_y_ = 0;
  int drag_origin_work_panel_height_ = 0;
  int drag_origin_agent_strip_rows_ = 0;
  std::optional<std::chrono::steady_clock::time_point> quit_arm_deadline_;
  std::size_t quit_arm_generation_ = 0;
  shared::AgentMetrics session_metrics_;
  std::jthread quit_arm_thread_;
  std::chrono::steady_clock::time_point last_live_raf_request_{};
  std::string live_row_last_phrase_key_;
  std::string live_row_current_phrase_;
  std::string live_row_previous_phrase_;
  std::string live_row_pending_phrase_key_;
  std::string live_row_pending_phrase_;
  std::chrono::steady_clock::time_point live_row_phrase_transition_started_at_{};
  std::chrono::steady_clock::time_point live_row_phrase_next_switch_at_{};
  std::chrono::steady_clock::time_point live_row_phrase_min_visible_until_{};
  uint64_t live_row_phrase_rng_state_ = 0x9e3779b97f4a7c15ULL;
  std::optional<std::string> pending_profile_modal_name_;
  std::jthread animation_tick_thread_;
  std::unordered_set<std::string> painted_profile_modals_;
public:
  SkinConfig skin_config_ = defaultSkinConfig(SkinKind::Firmius);
  void handleAppEvent(const shared::AppEvent &ev);
};

} // namespace firmius::tui

#endif
