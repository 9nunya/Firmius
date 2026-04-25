#ifndef FIRMIUS_TUI_STATE_HPP
#define FIRMIUS_TUI_STATE_HPP

#include "components/AgentStrip.hpp"
#include "components/PlanLane.hpp"
#include "components/TodoLane.hpp"
#include "components/ContextLane.hpp"
#include "EventQueue.hpp"
#include "Events.hpp"
#include "ActivePlanState.hpp"
#include "StreamStateManager.hpp"
#include "NotificationManager.hpp"
#include "WorkPanelLayout.hpp"
#include "models/TUIStore.hpp"
#include "models/TranscriptModel.hpp"
#include "models/PermissionModel.hpp"
#include "views/MainView.hpp"
#include "persistence/ThreadManager.hpp"
#include "SkinConfig.hpp"
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

enum class RefreshFlags : unsigned int {
  None = 0,
  Status = 1u << 0,
  AgentStrip = 1u << 1,
  PlanLane = 1u << 2,
  TodoLane = 1u << 3,
  ContextLane = 1u << 4,
  ChatTranscript = 1u << 5,
};

namespace detail {
bool shouldNotifyHiddenChatError(const std::string &focused_id, const std::string &error_id, bool hide_errors);
}

std::size_t BuildFocusedChatLiveMeasurementSignature(
    const StreamStateManager &stream_state, const std::string &focused_agent_id,
    const std::string &thread_id, const std::unordered_set<std::string> &persisted_tool_call_ids);

std::vector<shared::AgentTurn> expandCompactionTranscriptForDisplay(
    const std::vector<shared::AgentTurn>& turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>& snapshots);

class TuiState {
  friend class AppController;
public:
  static TuiState &instance();

  TuiState(const TuiState &) = delete;
  TuiState &operator=(const TuiState &) = delete;

  void init(firmius::core::Harness &harness,
            const shared::ThreadMetadata &thread,
            const std::string &focused_agent_id);
  void initModels();
  void attachScreen(ftxui::ScreenInteractive *screen);
  void shutdown();

  InputBarModel& getInputBarModel();
  void clearInputBuffer();
  bool handleCtrlC();
  void requestQuit();
  bool isQuitArmed() const;
  std::string exitSummaryText() const;

  ftxui::Component root();

  enum class ViewMode { Welcome, Chat, ProcessFocus };
  void setViewMode(ViewMode mode);
  ViewMode getViewMode() const;

  void openModal(const std::string &name);
  void openModalDirect(ftxui::Component modal, const std::string &modal_name = "");
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

  void loadUserPreferences();
  void persistUserPreferences() const;
  void syncCurrentThreadMetadataFromHarness(bool preserve_live_state);

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
  ftxui::Component chat_component_; 
  ftxui::Component root_component_;
  
  std::vector<ftxui::Component> modals_; 
  std::vector<std::function<void()>> deferred_ui_mutations_;
  
  ftxui::ScreenInteractive *screen_ = nullptr;
  firmius::shared::EventQueue<shared::AppEvent> event_queue_;
  int subscription_id_ = -1;
  void drainEvents();
  bool custom_event_pending_ = false;

  std::unordered_map<std::string, std::shared_ptr<shared::AgentHistory>> agent_history_cache_;
  std::unordered_map<std::string, std::unordered_set<std::string>> agent_persisted_tool_call_ids_cache_;
  
  std::shared_ptr<MainView> main_view_;

private:
  TuiState();
  ~TuiState() = default;

  void onEvent(const shared::AppEvent &ev);
  void requestRefresh(RefreshFlags flags);
  void applyPendingRefreshes();
  void updateStatusModel();
  void updateAgentStripModel();
  void updatePlanLaneModel();
  void updateContextLaneModel();
  void notifyChatTranscriptChanged();
  void refreshFocusedHistory();
  void rebuildEditableUserMessages();
  bool isEditModeSelection(uint64_t timestamp) const;
  void selectEditableMessageByTimestamp(uint64_t timestamp);
  bool commitSelectedEditableMessageToInput();
  void triggerTranscriptUndoFromHotkey();
  void triggerTranscriptRedoFromHotkey();
  void triggerEditUndoFromHotkey();
  void triggerEditRedoFromHotkey();

public:
  SkinConfig skin_config_ = defaultSkinConfig(SkinKind::Firmius);
  void handleAppEvent(const shared::AppEvent &ev);
};

void noteTuiModalOpenRequested(const std::string &name);

} // namespace firmius::tui

#endif
