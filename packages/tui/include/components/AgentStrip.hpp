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
  std::string status_text;
  float context_percent = 0.0f;
  bool is_busy = false;
  bool is_focused = false;
  int tool_call_count = 0;
  std::optional<uint64_t> working_since_ms;
};

inline constexpr size_t kAgentStripVisibleRows = 3;

struct AgentStripModel {
  std::vector<AgentStripItem> items;
  size_t view_offset = 0;
};

ftxui::Component AgentStrip(const std::shared_ptr<AgentStripModel> &model);

} // namespace firmius::tui

#endif
