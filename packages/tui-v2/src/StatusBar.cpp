#include "StatusBar.hpp"
#include "Terminal.hpp"

namespace firmius::tui2 {

StatusBar::StatusBar(const AppState &state) : state_(state) {}

int StatusBar::height(int /*width*/) const { return 2; }

std::vector<std::string> StatusBar::render(int width) const {
  return {renderLiveRow(width), renderHudRow(width)};
}

std::string StatusBar::renderLiveRow(int width) const {
  std::string content;

  auto status = state_.agentStatus();
  auto tools = state_.activeToolCalls();

  if (status == firmius::shared::AgentStatus::Streaming) {
    if (!tools.empty()) {
      content = " ⚙ " + tools.back().toolName + "...";
    } else {
      content = " ✦ Generating...";
    }
  } else if (!tools.empty()) {
    content = " ⚙ " + tools.back().toolName;
  } else {
    content = "";
  }

  if (content.empty()) {
    return std::string(width, ' ');
  }

  return ansi::bgRgb(25, 25, 35,
                     ansi::fg(245, ansi::fitToWidth(content, width)));
}

std::string StatusBar::renderHudRow(int width) const {
  // Connection indicator.
  std::string conn;
  switch (state_.connectionStatus()) {
  case ConnectionStatus::Connected:
    conn = ansi::fgRgb(100, 220, 100, "●");
    break;
  case ConnectionStatus::Connecting:
    conn = ansi::fgRgb(220, 180, 60, "○");
    break;
  case ConnectionStatus::Disconnected:
    conn = ansi::fgRgb(220, 60, 60, "○");
    break;
  }

  // Agent status.
  std::string statusLabel;
  switch (state_.agentStatus()) {
  case firmius::shared::AgentStatus::Streaming:
    statusLabel = ansi::fgRgb(100, 180, 255, "STREAMING");
    break;
  case firmius::shared::AgentStatus::Idle:
  default:
    statusLabel = ansi::fgRgb(120, 120, 140, "IDLE");
    break;
  }

  // Model.
  auto model = state_.modelLabel();
  std::string modelSeg =
      model.empty() ? ansi::dim("no model") : ansi::fgRgb(180, 160, 220, model);

  // Thread title.
  auto title = state_.threadTitle();
  std::string threadSeg = title.empty() ? ansi::dim("no thread")
                                        : ansi::fgRgb(200, 200, 210, title);

  // Agent purpose.
  auto purpose = state_.agentPurpose();
  std::string purposeSeg =
      purpose.empty() ? ""
                      : ansi::fgRgb(150, 180, 200, purpose) + ansi::dim(" │ ");

  // Context window.
  auto ctx = state_.agentContextWindow();
  std::string ctxSeg =
      ctx.empty() ? "" : ansi::fgRgb(120, 150, 120, ctx) + ansi::dim(" │ ");

  // NVIM-style: inverted segments separated by arrows.
  std::string bar = " " + conn + " " + statusLabel + ansi::dim(" │ ") +
                    threadSeg + ansi::dim(" │ ") + purposeSeg + ctxSeg +
                    modelSeg;

  // Queued messages indicator.
  int queued = state_.queuedMessageCount();
  if (queued > 0) {
    bar += ansi::dim(" │ ") +
           ansi::fgRgb(220, 180, 60, "[" + std::to_string(queued) + " queued]");
  }

  return ansi::bgRgb(30, 30, 42, ansi::fitToWidth(bar, width));
}

} // namespace firmius::tui2
