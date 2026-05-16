#pragma once

#include "TranscriptItem.hpp"
#include "utils/ToolView.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace firmius::tui2 {

/// A tool call item that owns its full lifecycle from Preparing to Finished.
/// Delegates rendering to ToolPresenterRegistry.
class ToolCallItem : public TranscriptItem {
public:
  ToolCallItem(std::string toolCallId, std::string toolName, std::string agentId);

  std::string_view type() const override { return "ToolCall"; }
  std::vector<std::string> render(int width) const override;
  int rowCount(int width) const override;
  bool isFinalized() const override { return !live_ && phase_ != ToolPhase::Preparing; }

  // State transitions (managed by EventRouter via ToolCallState)
  void setPhase(ToolPhase phase);
  void setArgs(std::string args);
  void setResult(bool success, std::string result);
  void addDiffEdit(firmius::shared::FileEditSignal edit);

  // Process tracking
  void setProcessId(std::string processId);
  void appendProcessOutput(const std::string& output, bool isStderr);
  void setProcessExitInfo(int exitCode, double durationMs);

  // Process exit info (from AgentProcessOutput finished=true)
  int processExitCode() const { return processExitCode_; }
  double processDurationMs() const { return processDurationMs_; }
  bool processExitKnown() const { return processExitKnown_; }

  // Accessors
  ToolPhase phase() const { return phase_; }
  const std::string& toolCallId() const { return toolCallId_; }
  const std::string& toolName() const { return toolName_; }
  const std::string& agentId() const { return agentId_; }
  const std::string& args() const { return args_; }
  const std::string& result() const { return result_; }
  const std::string& processId() const { return processId_; }
  const std::string& processStdout() const { return processStdout_; }
  const std::string& processStderr() const { return processStderr_; }
  const std::vector<firmius::shared::FileEditSignal>& diffEdits() const { return diffEdits_; }
  bool success() const { return success_; }

  // Live state
  bool isLive() const { return live_; }
  void setLive(bool live);
  std::chrono::milliseconds elapsed() const;

  // Expand/collapse
  bool isExpanded() const { return expanded_; }
  void setExpanded(bool expanded);

private:
  std::string toolCallId_;
  std::string toolName_;
  std::string agentId_;
  ToolPhase phase_ = ToolPhase::Preparing;
  std::string args_;
  std::string result_;
  bool success_ = false;
  std::vector<firmius::shared::FileEditSignal> diffEdits_;
  std::string processId_;
  std::string processStdout_;
  std::string processStderr_;
  int processExitCode_ = 0;
  double processDurationMs_ = 0.0;
  bool processExitKnown_ = false;
  bool live_ = false;
  bool expanded_ = false;
  std::chrono::steady_clock::time_point calledAt_;
};

} // namespace firmius::tui2
