#pragma once

#include "Enums.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::tui2 {

// ── Tool Call Lifecycle ──

/// High-level phase of a tool call, tracked by the TUI client.
/// Maps to ToolPhase for rendering, but has more granular states.
enum class ToolCallPhase {
  Streaming,    // AgentToolCallChunk arriving, name/args assembling
  Prepared,     // AgentToolCall received, args complete, not yet executed
  Executing,    // Tool is running (process spawned, permission pending, etc.)
  FinishedOk,   // setResult(true, ...)
  FinishedErr,  // setResult(false, ...)
};

/// Structured state for a single tool call, tracked from first chunk to finish.
struct ToolCallState {
  std::string toolCallId;
  std::string toolName;
  std::string agentId;
  std::string args;               // Final args (set from AgentToolCall)

  ToolCallPhase phase = ToolCallPhase::Streaming;
  std::string result;             // Set when finished
  bool success = false;

  // Process tracking
  std::string processId;          // Set from AgentProcessSpawned
  std::string processStdout;      // Accumulated stdout from AgentProcessOutput
  std::string processStderr;      // Accumulated stderr from AgentProcessOutput
  int processExitCode = 0;        // Set when process finishes
  double processDurationMs = 0.0; // Set when process finishes
  bool processFinished = false;   // True when process exited (result still pending)

  // Timing
  std::chrono::steady_clock::time_point createdAt{};
  std::chrono::steady_clock::time_point calledAt;   // When phase moved to Prepared
  std::chrono::steady_clock::time_point finishedAt;

  // Permission
  std::string pendingPermissionId;  // If waiting for permission

  // UI state
  bool expanded = false;

  // Streaming assembly (from AgentToolCallChunk)
  std::string nameAccum;
  std::string argsAccum;

  // ── Queries ──

  bool isFinished() const {
    return phase == ToolCallPhase::FinishedOk || phase == ToolCallPhase::FinishedErr;
  }

  bool isInflight() const {
    return !isFinished();
  }

  bool hasProcess() const {
    return !processId.empty();
  }

  std::chrono::milliseconds elapsed() const {
    if (calledAt == std::chrono::steady_clock::time_point{}) return {};
    auto end = isFinished() ? finishedAt : std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - calledAt);
  }
};

// ── Agent Turn Tracking ──

/// Tracks a single agent turn (one LLM response cycle).
struct AgentTurnState {
  std::string turnId;
  std::string agentId;

  // Tool calls in this turn (ordered)
  std::vector<std::string> toolCallIds;

  // Completion
  bool completed = false;
  std::string stopReason;
};

// ── Agent State ──

/// Per-agent state tracked by the TUI client.
struct AgentState {
  std::string agentId;
  std::string parentId;
  std::string personaName;
  std::string friendlyName;
  std::string title;
  std::string providerId;
  std::string modelId;
  uint32_t contextWindowTokens = 0;
  uint32_t contextUsedTokens = 0;
  uint32_t contextSentTokens = 0;

  firmius::shared::AgentStatus status = firmius::shared::AgentStatus::Idle;
  bool running = false;
  bool booting = false;

  // Current turn
  std::optional<AgentTurnState> currentTurn;
};

} // namespace firmius::tui2
