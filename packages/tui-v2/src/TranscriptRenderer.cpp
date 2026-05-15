#include "TranscriptRenderer.hpp"

#include <algorithm>

namespace firmius::tui2 {

TranscriptRenderer::TranscriptRenderer(Terminal& terminal)
    : terminal_(terminal) {}

void TranscriptRenderer::renderSnapshot(
    const std::vector<TranscriptLine>& lines, int width) {
  for (const auto& line : lines) {
    std::string formatted = formatLine(line, width);
    terminal_.pushLine(formatted);
  }
}

void TranscriptRenderer::renderDelta(
    const std::vector<TranscriptLine>& lines,
    size_t fromIndex, int width) {
  for (size_t i = fromIndex; i < lines.size(); ++i) {
    std::string formatted = formatLine(lines[i], width);
    terminal_.pushLine(formatted);
  }
}

void TranscriptRenderer::renderStreamingText(
    const std::string& text, int /*width*/, int /*scrollBottom*/) {
  if (text.empty()) return;

  // For progressive streaming, we push the latest text chunk as a line.
  // A future improvement would update the last scroll line in-place.
  // For now, streaming text is aggregated via AppState and flushed
  // as a transcript line when finalized.
  (void)text;
}

std::string TranscriptRenderer::formatLine(
    const TranscriptLine& line, int /*width*/) {
  std::string content = line.text;
  std::string styled;

  switch (line.kind) {
  case TranscriptLine::Kind::UserMessage:
    styled = ansi::bold(ansi::fgRgb(100, 140, 220, "> " + content));
    break;
  case TranscriptLine::Kind::AssistantText:
    styled = ansi::fgRgb(220, 220, 230, content);
    break;
  case TranscriptLine::Kind::Thinking:
    styled = ansi::dim(ansi::fgRgb(160, 160, 180, "  " + content));
    break;
  case TranscriptLine::Kind::ToolCall:
    styled = ansi::fgRgb(220, 180, 80, "  * " + content);
    break;
  case TranscriptLine::Kind::ToolResult:
    styled = line.success
        ? ansi::fgRgb(100, 200, 120, "  + " + content)
        : ansi::fgRgb(220, 80, 80, "  x " + content);
    break;
  case TranscriptLine::Kind::Notice:
    styled = ansi::fgRgb(220, 160, 60, "  ! " + content);
    break;
  case TranscriptLine::Kind::System:
    styled = ansi::dim(ansi::fgRgb(120, 120, 140, "  " + content));
    break;
  }

  return styled;
}

std::vector<TranscriptLine> TranscriptRenderer::turnsToLines(
    const std::vector<firmius::shared::AgentTurn>& turns, int /*width*/) {
  std::vector<TranscriptLine> lines;

  for (const auto& turn : turns) {
    for (const auto& msg : turn.messages) {
      // Skip system prompts — they're not user-facing content.
      if (msg.role == firmius::shared::Role::System) continue;

      for (const auto& part : msg.content) {
        TranscriptLine line;

        if (const auto* text = std::get_if<firmius::shared::TextContent>(&part)) {
          if (msg.role == firmius::shared::Role::User) {
            line.kind = TranscriptLine::Kind::UserMessage;
          } else {
            line.kind = TranscriptLine::Kind::AssistantText;
          }
          // Split multiline text into separate lines.
          std::string remaining = text->text;
          size_t pos = 0;
          while ((pos = remaining.find('\n')) != std::string::npos) {
            TranscriptLine subLine;
            subLine.kind = line.kind;
            subLine.text = remaining.substr(0, pos);
            if (!subLine.text.empty()) {
              lines.push_back(std::move(subLine));
            }
            remaining = remaining.substr(pos + 1);
          }
          if (!remaining.empty()) {
            TranscriptLine subLine;
            subLine.kind = line.kind;
            subLine.text = remaining;
            lines.push_back(std::move(subLine));
          }
          continue;
        }

        if (const auto* thinking = std::get_if<firmius::shared::ThinkingContent>(&part)) {
          line.kind = TranscriptLine::Kind::Thinking;
          line.text = thinking->thinking;
          if (!line.text.empty()) {
            lines.push_back(std::move(line));
          }
          continue;
        }

        if (const auto* toolCall = std::get_if<firmius::shared::ToolCallContent>(&part)) {
          line.kind = TranscriptLine::Kind::ToolCall;
          line.toolCallId = toolCall->id;
          line.toolName = toolCall->name;
          line.text = toolCall->name;
          lines.push_back(std::move(line));
          continue;
        }

        if (const auto* toolResult = std::get_if<firmius::shared::ToolResultContent>(&part)) {
          line.kind = TranscriptLine::Kind::ToolResult;
          line.toolCallId = toolResult->toolCallId;
          line.success = toolResult->success;
          line.text = toolResult->toolCallId;
          lines.push_back(std::move(line));
          continue;
        }

        if (const auto* error = std::get_if<firmius::shared::ErrorContent>(&part)) {
          line.kind = TranscriptLine::Kind::Notice;
          line.text = error->errorName + ": " + error->description;
          lines.push_back(std::move(line));
          continue;
        }

        if (const auto* notice = std::get_if<firmius::shared::NoticeContent>(&part)) {
          line.kind = TranscriptLine::Kind::Notice;
          line.text = notice->title + ": " + notice->message;
          lines.push_back(std::move(line));
          continue;
        }
      }
    }
  }

  return lines;
}

} // namespace firmius::tui2
