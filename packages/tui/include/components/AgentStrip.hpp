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
  float context_percent;
  bool is_busy;
};

struct AgentStripModel {
  std::vector<AgentStripItem> items;
};

ftxui::Component AgentStrip(const std::shared_ptr<AgentStripModel> &model);

} // namespace firmius::tui

#endif
