#ifndef FIRMIUS_SHARED_UTILS_TOOL_VIEW_HPP
#define FIRMIUS_SHARED_UTILS_TOOL_VIEW_HPP

#include <string>
#include <vector>

namespace firmius::shared {

enum class ToolPhase {
  Preparing,
  Called,
  Finished,
};

struct ToolCallView {
  std::string agentId;
  std::string toolCallId;
  std::string name;
  std::string args;
  std::string result;
  bool success = false;
  ToolPhase phase = ToolPhase::Preparing;
  bool show_result = false;
  std::string toggle_label = "show";
  std::string live_process_output;
  std::vector<std::string> subagent_tool_log;
  std::string subagent_title;
  bool subagent_running = false;
  std::string last_subagent_tool_id;
  std::string subagent_slug;
};

} // namespace firmius::shared

#endif
