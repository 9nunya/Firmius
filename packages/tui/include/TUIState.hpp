#ifndef FIRMIUS_TUI_STATE_HPP
#define FIRMIUS_TUI_STATE_HPP

#include "Context.hpp"
#include "EventQueue.hpp"
#include "Events.hpp"
#include "ActivePlanState.hpp"
#include "StreamStateManager.hpp"
#include "NotificationManager.hpp"
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::core {
class Harness;
}

namespace firmius::tui {

struct TitleBarModel;
struct StatusBarModel;
struct InputBarModel;
struct AgentStripModel;
struct PlanLaneModel;

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

  std::string getProcessOutput(const std::string &pid);
  std::string getSubagentOutput(const std::string &subagentId);

  ftxui::Component root();

  enum class ViewMode { Welcome, Chat, ProcessFocus };
  void setViewMode(ViewMode mode);
  ViewMode getViewMode() const;

  void openModal(const std::string &name);
  void openModalDirect(ftxui::Component modal);
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

private:
  TuiState();
  ~TuiState() = default;

  void onEvent(const shared::AppEvent &ev);
  std::string statusText() const;
  void updateStatusModel();
  void updateAgentStripModel();
  void updatePlanLaneModel();
  std::optional<shared::Plan> loadActivePlanForThread(
      const shared::ThreadMetadata &thread) const;

  firmius::core::Harness *harness_ = nullptr;
  shared::ThreadMetadata thread_;
  std::string focused_agent_id_;
  std::shared_ptr<shared::AgentHistory> history_;
  int subscription_id_ = -1;
  ftxui::ScreenInteractive *screen_ = nullptr;

  firmius::shared::EventQueue<shared::AppEvent> event_queue_;
  void drainEvents();

  StreamStateManager stream_state_;

  std::string input_;
  int cursor_ = 0;

  std::shared_ptr<TitleBarModel> title_model_;
  std::shared_ptr<StatusBarModel> status_model_;
  std::shared_ptr<InputBarModel> input_model_;
  std::shared_ptr<AgentStripModel> agent_strip_model_;
  std::shared_ptr<PlanLaneModel> plan_lane_model_;
  ActivePlanState active_plan_state_;

  ViewMode view_mode_ = ViewMode::Chat;
  std::vector<ftxui::Component> modals_; // Used as a stack
  std::vector<std::function<void()>> deferred_ui_mutations_;
  bool pending_modal_clear_ =
      false; // Deferred clear to avoid UB in modal handlers
  bool diffs_expanded_ = true; // Ctrl+G toggle for diff expansion
  bool plan_lane_expanded_ = false;

  ftxui::Component root_component_;
  ftxui::Component chat_component_;
  ftxui::Component input_component_;
  std::unordered_map<std::string, uint64_t> agent_work_start_ms_;
  
  // Process focus mode
  std::string focused_process_id_;
  bool process_focus_expanded_ = false;
};

} // namespace firmius::tui

#endif
