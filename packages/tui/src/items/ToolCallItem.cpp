#include "items/ToolCallItem.hpp"
#include "tools/IToolPresenter.hpp"
#include "tools/ToolPresenterRegistry.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include "utils/ToolView.hpp"

namespace firmius::tui {

ToolCallItem::ToolCallItem(std::string toolCallId, std::string toolName, std::string agentId)
    : toolCallId_(std::move(toolCallId)),
      toolName_(std::move(toolName)),
      agentId_(std::move(agentId)),
      calledAt_(std::chrono::steady_clock::now()) {}

void ToolCallItem::setPhase(ToolPhase phase) {
  if (phase_ != phase) {
    phase_ = phase;
    if (phase == ToolPhase::Called || phase == ToolPhase::Preparing) {
      calledAt_ = std::chrono::steady_clock::now();
    }
    touch();
  }
}

void ToolCallItem::setArgs(std::string args) {
  args_ = std::move(args);
  // No hidden phase transition — EventRouter controls phase explicitly.
  touch();
}

void ToolCallItem::setProcessId(std::string processId) {
  processId_ = std::move(processId);
  touch();
}

void ToolCallItem::setSubagentId(std::string subagentId) {
  subagentId_ = std::move(subagentId);
  touch();
}

void ToolCallItem::appendProcessOutput(const std::string& output, bool isStderr) {
  if (isStderr) {
    processStderr_ += output;
  } else {
    processStdout_ += output;
  }
  touch();
}

void ToolCallItem::setProcessExitInfo(int exitCode, double durationMs) {
  processExitCode_ = exitCode;
  processDurationMs_ = durationMs;
  processExitKnown_ = true;
  touch();
}

void ToolCallItem::setResult(bool success, std::string result) {
  success_ = success;
  result_ = std::move(result);
  phase_ = success ? ToolPhase::FinishedSuccess : ToolPhase::FinishedError;
  live_ = false;
  touch();
}

void ToolCallItem::addDiffEdit(firmius::shared::FileEditSignal edit) {
  diffEdits_.push_back(std::move(edit));
  touch();
}

void ToolCallItem::setLive(bool live) {
  if (live_ != live) {
    live_ = live;
    touch();
  }
}

std::chrono::milliseconds ToolCallItem::elapsed() const {
  if (phase_ == ToolPhase::Preparing) return std::chrono::milliseconds(0);
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - calledAt_);
}

void ToolCallItem::setExpanded(bool expanded) {
  if (expanded_ != expanded) {
    expanded_ = expanded;
    touch();
  }
}

std::vector<std::string> ToolCallItem::render(int width) const {
  auto* presenter = ToolPresenterRegistry::instance().find(toolName_);
  if (presenter) {
    ToolRenderContext ctx;
    ctx.state = appState_;
    return presenter->render(*this, ctx, width);
  }
  // Fallback: simple one-liner
  std::string prefix;
  switch (phase_) {
  case ToolPhase::Preparing:
    prefix = theme_ansi::warning("  \xe2\x9a\x99 " + toolName_);
    break;
  case ToolPhase::Called:
    prefix = theme_ansi::warning("  \xe2\x9a\x99 " + toolName_);
    break;
  case ToolPhase::FinishedSuccess:
    prefix = theme_ansi::success("  \xe2\x9c\x93 " + toolName_);
    break;
  case ToolPhase::FinishedError:
    prefix = theme_ansi::error("  \xe2\x9c\x97 " + toolName_);
    break;
  }
  return {prefix};
}

int ToolCallItem::rowCount(int width) const {
  // Default: 1 line. Presenters override via render() returning more lines.
  // For accurate count, we call render() and count. This is cached by the
  // dirty-tracking system so it's not called on every frame.
  return static_cast<int>(render(width).size());
}

} // namespace firmius::tui
