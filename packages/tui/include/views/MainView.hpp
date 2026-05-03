#ifndef FIRMIUS_TUI_VIEWS_MAIN_VIEW_HPP
#define FIRMIUS_TUI_VIEWS_MAIN_VIEW_HPP

#include "models/TUIStore.hpp"
#include "models/TranscriptModel.hpp"
#include "models/PermissionModel.hpp"
#include "WorkPanelLayout.hpp"
#include <unordered_set>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component.hpp>
#include <memory>

namespace firmius::tui {

class MainView : public ftxui::ComponentBase {
public:
  MainView();
  
  ftxui::Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;

  bool HandleGlobalHotkeys(ftxui::Event event);
  static const std::unordered_set<std::string>& persistedToolCallIds();

  void setSelectedWorkPanelTab(WorkPanelTab tab) { selected_work_panel_tab_ = tab; }
  WorkPanelTab getSelectedWorkPanelTab() const { return selected_work_panel_tab_; }
  void setShowWorkPanel(bool show) { show_work_panel_ = show; }
  bool getShowWorkPanel() const { return show_work_panel_; }
  void setShowAgentStrip(bool show) { show_agent_strip_ = show; }
  bool getShowAgentStrip() const { return show_agent_strip_; }

  ftxui::Component getChatComponent() { return chat_component_; }
  ftxui::Component getInputComponent() { return input_component_; }

private:
  ftxui::Component chat_component_;
  ftxui::Component input_component_;
  ftxui::Component plan_lane_;
  ftxui::Component todo_lane_;
  ftxui::Component context_lane_;
  ftxui::Component agent_strip_;
  ftxui::Component title_bar_;
  ftxui::Component status_bar_;
  ftxui::Component live_status_row_;
  ftxui::Component welcome_screen_;

  ftxui::Box work_panel_separator_box_;
  ftxui::Box agent_strip_separator_box_;

  [[maybe_unused]] int last_terminal_width_ = 0;
  [[maybe_unused]] int last_terminal_height_ = 0;
  bool show_agent_strip_ = true;
  bool show_work_panel_ = true;
  int work_panel_height_override_ = 0;
  bool help_opened_from_empty_query_ = false;
  bool command_palette_requested_ = false;
  int agent_strip_visible_rows_ = 4;
  WorkPanelTab selected_work_panel_tab_ = WorkPanelTab::Context;

  // Drag state
  bool dragging_work_panel_ = false;
  bool dragging_agent_strip_ = false;
  int drag_origin_y_ = 0;
  int drag_origin_value_ = 0;
};

// Helper for focus candidates
std::vector<std::string> focusCycleCandidates(const std::string &focusedAgentId);
std::vector<std::string> getSwitchableLeadPersonas();
std::shared_ptr<MainView> MakeMainView();

} // namespace firmius::tui

#endif
