#ifndef FIRMIUS_TUI_TOOLS_SUBAGENT_STATE_HPP
#define FIRMIUS_TUI_TOOLS_SUBAGENT_STATE_HPP

#include "utils/ToolView.hpp"
#include <string>
#include <vector>

namespace firmius::tui {

enum class SubagentOutcomeKind {
  Unknown,
  Response,
  NoSummary,
  Cancelled,
  Failed,
};

struct NormalizedSubagentState {
  std::string parent_tool_call_id;
  std::string owner_agent_id;
  std::string child_agent_id;
  std::string child_title;
  std::string child_friendly_name;
  std::string task;

  bool running = false;
  bool waiting = false;
  bool provider_waiting = false;
  bool retrying = false;
  bool account_switched = false;
  bool fallback_used = false;

  std::string wait_state;
  std::string route_category;
  std::vector<std::string> attempted_categories;
  SubagentOutcomeKind outcome = SubagentOutcomeKind::Unknown;
  std::string final_summary;
  std::string error_text;

  std::vector<std::string> artifacts_created;
  std::vector<std::string> artifacts_updated;

  std::vector<firmius::shared::SubagentToolLogEntry> activity_log;
};

} // namespace firmius::tui

#endif
