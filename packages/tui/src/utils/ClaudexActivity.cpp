#include "utils/ClaudexActivity.hpp"
#include "utils/ToolView.hpp"
#include <algorithm>

namespace firmius::tui {

std::string inferClaudexActivity(const firmius::shared::AgentContext &ctx,
                                 const firmius::tui::StreamState *stream,
                                 const std::string &fallback,
                                 const firmius::shared::AgentTodoList *todo,
                                 const std::string &status_text) {
  // Provider waiting or active thinking/streaming are top priority.
  if (stream && (stream->provider_waiting || stream->is_thinking || !stream->text.empty() || !stream->thinking.empty())) {
    return stream->provider_waiting ? "waiting" : "thinking";
  }

  // Finishing mode: >20 turns and all todo items done
  if (ctx.history && ctx.history->turns.size() > 20 && todo && !todo->items.empty()) {
    const bool all_done = std::all_of(todo->items.begin(), todo->items.end(),
                                     [](const auto &item) {
                                       return item.status == firmius::shared::TodoStatus::Done;
                                     });
    if (all_done) {
      return "finishing";
    }
  }

  // Orchestrating mode: waiting on agents
  if (status_text.find("subagent") != std::string::npos ||
      status_text.find("delegate") != std::string::npos) {
    return "orchestrating";
  }

  // Exploring mode: recently doing reads and searches
  if (status_text.find("read") != std::string::npos ||
      status_text.find("search") != std::string::npos ||
      status_text.find("grep") != std::string::npos ||
      status_text.find("glob") != std::string::npos) {
    return "exploring";
  }
  // If the agent is currently preparing or executing a file-edit-like tool,
  // prioritize the "editing" mode.
  if (fallback.find("Edit") != std::string::npos || fallback.find("file_edit") != std::string::npos) {
    return "editing";
  }

  // If there are any blocking/owned processes, the agent is effectively
  // verifying/working, even if the LLM isn't actively streaming.
  if (!ctx.state.blockingProcessIds.empty() || !ctx.state.ownedProcesses.empty()) {
    return "verifying";
  }

  // Translate common status bar fallbacks into Claudex modes.
  if (fallback == "streaming" || fallback == "thinking")
    return "thinking";
  if (fallback == "executing_tool" || fallback == "working")
    return "working";
  if (fallback == "verifying")
    return "verifying";
  if (fallback == "provider_waiting" || fallback == "waiting")
    return "waiting";

  return fallback.empty() ? std::string("idle") : fallback;
}

} // namespace firmius::tui
