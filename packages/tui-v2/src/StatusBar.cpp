#include "StatusBar.hpp"
#include "items/ToolCallItem.hpp"
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

  // Find the last active tool call item
  const ToolCallItem* lastTool = nullptr;
  for (const auto& item : state_.items()) {
    if (item->type() == "ToolCall") {
      auto* tc = static_cast<const ToolCallItem*>(item.get());
      if (tc->phase() == ToolPhase::Called || tc->phase() == ToolPhase::Preparing) {
        lastTool = tc;
      }
    }
  }

  if (status == firmius::shared::AgentStatus::Streaming) {
    if (lastTool) {
      content = " \xe2\x9a\x99 " + lastTool->toolName() + "...";
    } else {
      content = " \xe2\x9c\xa6 Generating...";
    }
  } else if (lastTool) {
    content = " \xe2\x9a\x99 " + lastTool->toolName();
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
  std::string conn;
  switch (state_.connectionStatus()) {
  case ConnectionStatus::Connected:
    conn = ansi::fgRgb(100, 220, 100, "\xe2\x97\x8f");
    break;
  case ConnectionStatus::Connecting:
    conn = ansi::fgRgb(220, 180, 60, "\xe2\x97\x8b");
    break;
  case ConnectionStatus::Disconnected:
    conn = ansi::fgRgb(220, 60, 60, "\xe2\x97\x8b");
    break;
  }

  std::string statusLabel;
  switch (state_.agentStatus()) {
  case firmius::shared::AgentStatus::Streaming:
    statusLabel = ansi::fgRgb(100, 180, 255, "STREAMING");
    break;
  case firmius::shared::AgentStatus::ExecutingTool:
    statusLabel = ansi::fgRgb(220, 180, 60, "EXEC TOOL");
    break;
  case firmius::shared::AgentStatus::ProviderWaiting:
    statusLabel = ansi::fgRgb(160, 160, 255, "WAITING");
    break;
  case firmius::shared::AgentStatus::Compacting:
    statusLabel = ansi::fgRgb(180, 140, 255, "COMPACTING");
    break;
  case firmius::shared::AgentStatus::AwaitingInput:
    statusLabel = ansi::fgRgb(100, 220, 180, "AWAITING INPUT");
    break;
  case firmius::shared::AgentStatus::Error:
    statusLabel = ansi::fgRgb(255, 80, 80, "ERROR");
    break;
  case firmius::shared::AgentStatus::Cancelled:
    statusLabel = ansi::fgRgb(200, 120, 60, "CANCELLED");
    break;
  case firmius::shared::AgentStatus::Idle:
  default:
    statusLabel = ansi::fgRgb(120, 120, 140, "IDLE");
    break;
  }

  auto model = state_.modelLabel();
  std::string modelSeg =
      model.empty() ? ansi::dim("no model") : ansi::fgRgb(180, 160, 220, model);

  auto title = state_.threadTitle();
  std::string threadSeg = title.empty() ? ansi::dim("no thread")
                                        : ansi::fgRgb(200, 200, 210, title);

  auto purpose = state_.agentPurpose();
  std::string purposeSeg =
      purpose.empty() ? ""
                      : ansi::fgRgb(150, 180, 200, purpose) + ansi::dim(" \xe2\x94\x82 ");

  auto ctx = state_.agentContextWindow();
  std::string ctxSeg =
      ctx.empty() ? "" : ansi::fgRgb(120, 150, 120, ctx) + ansi::dim(" \xe2\x94\x82 ");

  std::string bar = " " + conn + " " + statusLabel + ansi::dim(" \xe2\x94\x82 ") +
                    threadSeg + ansi::dim(" \xe2\x94\x82 ") + purposeSeg + ctxSeg +
                    modelSeg;

  int queued = state_.queuedMessageCount();
  if (queued > 0) {
    bar += ansi::dim(" \xe2\x94\x82 ") +
           ansi::fgRgb(220, 180, 60, "[" + std::to_string(queued) + " queued]");
  }

  return ansi::bgRgb(30, 30, 42, ansi::fitToWidth(bar, width));
}

} // namespace firmius::tui2
