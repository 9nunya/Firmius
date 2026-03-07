#ifndef FIRMIUS_COMPONENTS_TOOL_BLOCK_HPP
#define FIRMIUS_COMPONENTS_TOOL_BLOCK_HPP

#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

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
  std::vector<std::string>
      subagent_tool_log;      // Rolling log of subagent tool summaries
  std::string subagent_title; // Title from summon_subagent args
};

ftxui::Component ToolBlock(const std::shared_ptr<ToolCallView> &view);

} // namespace firmius::tui

#endif
