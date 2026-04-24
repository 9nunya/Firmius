#include "utils/ClaudexActivity.hpp"

namespace firmius::tui {

std::string inferClaudexActivity(const firmius::shared::AgentContext &ctx,
                                 const firmius::tui::StreamState *stream,
                                 const std::string &fallback) {
  // Provider waiting is a distinct UX state; keep it top priority.
  if (stream && stream->provider_waiting) {
    return "waiting";
  }

  // If there are any blocking/owned processes, the agent is effectively
  // verifying/working, even if the LLM isn't actively streaming.
  if (!ctx.state.blockingProcessIds.empty() || !ctx.state.ownedProcesses.empty()) {
    return "verifying";
  }

  // Translate common status bar fallbacks into Claudex modes.
  if (fallback == "streaming")
    return "thinking";
  if (fallback == "executing_tool")
    return "working";
  if (fallback == "provider_waiting")
    return "waiting";

  return fallback.empty() ? std::string("idle") : fallback;
}

} // namespace firmius::tui
