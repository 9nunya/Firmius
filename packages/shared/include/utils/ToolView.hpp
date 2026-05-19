#ifndef FIRMIUS_SHARED_TOOLVIEW_HPP
#define FIRMIUS_SHARED_TOOLVIEW_HPP

#include "utils/StringUtil.hpp"
#include <optional>
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

struct FileEditSignal {
  std::string path;
  std::string diffPreview;
  int addedLines = 0;
  int removedLines = 0;
};

struct ToolFollowUpNotice {
  std::string text;
  std::string hotkeyLabel;
  bool visible = false;
  bool dismissOnAgentResume = false;
  bool dismissOnMatchingRedo = false;

  bool operator==(const ToolFollowUpNotice &other) const = default;
};

struct ToolCallView {

  std::string agentId;
  std::string toolCallId;
  std::string name;
  std::string args;
  std::string result;
  std::string previous_result;
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
  std::string subagent_wait_state;
  std::string subagent_wait_message;
  bool subagent_fallback_used = false;
  std::string subagent_route_category;
  std::vector<std::string> subagent_attempted_categories;
  std::vector<FileEditSignal> fileEditEvents;
  std::optional<ToolFollowUpNotice> followUpNotice;
};

inline bool isFileEditLikeToolName(const std::string &name) {
  return name == "Edit" || name == "file_edit" || name == "file_write" ||
         name == "Write" || name == "EditWrite" || name == "EditReplace" ||
         name == "EditRange";
}

inline bool ToolCallHasRenderableIdentity(const std::string &tool_name) {
  return !StringUtil::trim(tool_name).empty();
}

inline bool ToolCallHasRenderableIdentity(const ToolCallView &view) {
  return ToolCallHasRenderableIdentity(view.name);
}

} // namespace firmius::shared

#endif
