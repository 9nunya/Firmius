#ifndef FIRMIUS_SHARED_UTILS_TOOL_VIEW_HPP
#define FIRMIUS_SHARED_UTILS_TOOL_VIEW_HPP

#include "utils/StringUtil.hpp"
#include <string>
#include <vector>

namespace firmius::shared {

enum class ToolPhase {
  Preparing,
  Called,
  BackgroundRunning,
  Finished,
  Error,
};

/**
 * @brief Represents a single tool call entry in a subagent's tool log.
 */
struct SubagentToolLogEntry {
  std::string summary;
  ToolPhase phase = ToolPhase::Preparing;
  std::string toolCallId;
  std::string name;  // Store tool name for summary regeneration
  std::string args;  // Store tool args for summary regeneration
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
  std::string process_id;
  bool process_is_background = false;
  bool process_exit_known = false;
  int process_exit_code = 0;
  double process_duration_ms = 0.0;
  std::vector<SubagentToolLogEntry> subagent_tool_log;
  std::string subagent_title;
  bool subagent_running = false;
  std::string subagent_slug;
  std::string subagent_id;
};

inline bool ToolCallHasRenderableIdentity(const std::string &tool_name) {
  return !StringUtil::trim(tool_name).empty();
}

inline bool ToolCallHasRenderableIdentity(const ToolCallView &view) {
  return ToolCallHasRenderableIdentity(view.name);
}

} // namespace firmius::shared

#endif
