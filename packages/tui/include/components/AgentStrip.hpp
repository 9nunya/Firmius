#ifndef FIRMIUS_COMPONENTS_AGENT_STRIP_HPP
#define FIRMIUS_COMPONENTS_AGENT_STRIP_HPP

#include <ftxui/component/component_base.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui {

struct AgentStripItem {
  std::string id;
  std::string title;
  std::string purpose;
  std::string model_name;
  std::string model_variant;
  std::string status_text;
  float context_percent = 0.0f;
  bool is_busy = false;
  bool is_focused = false;
  int tool_call_count = 0;
  std::optional<uint64_t> working_since_ms;
  int hierarchy_depth = 0;  // 0 = lead, 1 = subagent, 2 = sub-subagent, etc.
  std::string parent_id;    // Empty if lead agent
  bool has_children = false; // True if this agent has subagents
};

inline constexpr size_t kAgentStripVisibleRows = 4;

struct AgentStripModel {
  std::vector<AgentStripItem> items;
  size_t view_offset = 0;
  std::size_t layout_generation = 0;
  size_t visible_rows = kAgentStripVisibleRows;
  std::function<void(const std::string&)> on_item_click;
  std::function<void(int)> on_scroll_request;
};

ftxui::Component AgentStrip(const std::shared_ptr<AgentStripModel> &model);

} // namespace firmius::tui

#endif
