#ifndef FIRMIUS_COMPONENTS_AGENT_STRIP_HPP
#define FIRMIUS_COMPONENTS_AGENT_STRIP_HPP

#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

struct AgentStripItem {
  std::string id;
  std::string title;
  std::string purpose;
  std::string status_text;
  float context_percent = 0.0f;
  bool is_busy = false;
  bool is_focused = false;
  int tool_call_count = 0;
};

struct AgentStripModel {
  std::vector<AgentStripItem> items;
};

ftxui::Component AgentStrip(const std::shared_ptr<AgentStripModel> &model);

} // namespace firmius::tui

#endif
