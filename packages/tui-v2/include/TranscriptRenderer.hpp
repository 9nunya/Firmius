#pragma once

#include "AppState.hpp"
#include "Terminal.hpp"

#include <string>
#include <vector>

namespace firmius::tui2 {

/// Renders transcript content into the terminal scroll region.
class TranscriptRenderer {
public:
  explicit TranscriptRenderer(Terminal &terminal);

  /// Render a full transcript snapshot (for initial load / reconnect).
  void renderSnapshot(const std::vector<TranscriptLine> &lines, int width);

  /// Render new lines since the last render call.
  void renderDelta(const std::vector<TranscriptLine> &lines,
                   size_t fromIndex, int width);

  /// Render the current streaming text at the bottom of the scroll region.
  void renderStreamingText(const std::string &text, int width, int scrollBottom);

  /// Format a single transcript line for display (public for testing).
  static std::string formatLine(const TranscriptLine &line, int width);

  /// Convert AgentTurn messages into TranscriptLines.
  static std::vector<TranscriptLine> turnsToLines(
      const std::vector<firmius::shared::AgentTurn> &turns, int width);

private:
  Terminal &terminal_;
};

} // namespace firmius::tui2
