#ifndef FIRMIUS_TUI_TOOLS_PROCESS_STATE_HPP
#define FIRMIUS_TUI_TOOLS_PROCESS_STATE_HPP

#include <string>

namespace firmius::tui {

struct NormalizedProcessState {
  std::string process_id;
  std::string owner_agent_id;
  std::string origin_tool_call_id;
  std::string command;
  std::string cwd;
  bool running = false;
  bool finished = false;
  bool exit_code_known = false;
  int exit_code = 0;
  double duration_ms = 0.0;
  bool is_blocking = false;
  bool is_background = false;
  std::string latest_output_tail;
  std::string waiting_pattern;
  bool waiting = false;
  std::string wait_state;
};

} // namespace firmius::tui

#endif
