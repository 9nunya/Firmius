#ifndef FIRMIUS_TUI_STATE_HPP
#define FIRMIUS_TUI_STATE_HPP

#include "Context.hpp"
#include "EventQueue.hpp"
#include "Events.hpp"
#include "StreamStateManager.hpp"
#include "NotificationManager.hpp"
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
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

  void postEvent(ftxui::Event event);

private:
  TuiState();
  ~TuiState() = default;

  void onEvent(const shared::AppEvent &ev);
  std::string statusText() const;
  void updateStatusModel();
  void updateAgentStripModel();

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

  ViewMode view_mode_ = ViewMode::Chat;
  std::vector<ftxui::Component> modals_; // Used as a stack
  bool pending_modal_clear_ =
      false; // Deferred clear to avoid UB in modal handlers
  bool show_help_ = false;
  bool diffs_expanded_ = true; // Ctrl+G toggle for diff expansion

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
