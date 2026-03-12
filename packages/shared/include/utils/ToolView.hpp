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

/**
 * @brief Represents a single tool call entry in a subagent's tool log.
 */
struct SubagentToolLogEntry {
  std::string summary;
  ToolPhase phase = ToolPhase::Preparing;
  std::string toolCallId;
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
  std::vector<SubagentToolLogEntry> subagent_tool_log;
  std::string subagent_title;
  bool subagent_running = false;
  std::string last_subagent_tool_id;
  std::string subagent_slug;
  std::string subagent_id;
};

} // namespace firmius::shared

#endif
