#include "TranscriptRenderer.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace firmius::tui2 {

namespace {

std::string trim(std::string text) {
  auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
  text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
  return text;
}

std::string renderInlineMarkdown(const std::string& text) {
  std::string out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size();) {
    if (text[i] == '`') {
      size_t end = text.find('`', i + 1);
      if (end != std::string::npos) {
        out += ansi::bgRgb(45, 45, 55,
                           ansi::fgRgb(235, 235, 245, text.substr(i + 1, end - i - 1)));
        i = end + 1;
        continue;
      }
    }

    if (i + 1 < text.size() && text[i] == '*' && text[i + 1] == '*') {
      size_t end = text.find("**", i + 2);
      if (end != std::string::npos) {
        out += ansi::bold(renderInlineMarkdown(text.substr(i + 2, end - i - 2)));
        i = end + 2;
        continue;
      }
    }

    if (i + 1 < text.size() && text[i] == '_' && text[i + 1] == '_') {
      size_t end = text.find("__", i + 2);
      if (end != std::string::npos) {
        out += ansi::bold(renderInlineMarkdown(text.substr(i + 2, end - i - 2)));
        i = end + 2;
        continue;
      }
    }

    if (i + 1 < text.size() && text[i] == '~' && text[i + 1] == '~') {
      size_t end = text.find("~~", i + 2);
      if (end != std::string::npos) {
        out += ansi::strikethrough(text.substr(i + 2, end - i - 2));
        i = end + 2;
        continue;
      }
    }

    if (text[i] == '*') {
      size_t end = text.find('*', i + 1);
      if (end != std::string::npos) {
        out += ansi::italic(renderInlineMarkdown(text.substr(i + 1, end - i - 1)));
        i = end + 1;
        continue;
      }
    }

    if (text[i] == '_') {
      size_t end = text.find('_', i + 1);
      if (end != std::string::npos) {
        out += ansi::italic(renderInlineMarkdown(text.substr(i + 1, end - i - 1)));
        i = end + 1;
        continue;
      }
    }

    out.push_back(text[i++]);
  }

  return out;
}

std::string renderMarkdownBlock(const std::string& text) {
  std::stringstream input(text);
  std::string line;
  std::string out;
  bool inFence = false;

  while (std::getline(input, line)) {
    std::string stripped = trim(line);
    std::string rendered;

    if (stripped.rfind("```", 0) == 0) {
      inFence = !inFence;
      rendered = ansi::dim(ansi::fgRgb(150, 150, 170, stripped));
    } else if (inFence) {
      rendered = ansi::bgRgb(25, 25, 32, ansi::fgRgb(210, 210, 220, line));
    } else if (stripped.rfind("#", 0) == 0) {
      size_t hashes = 0;
      while (hashes < stripped.size() && stripped[hashes] == '#') ++hashes;
      if (hashes < stripped.size() && stripped[hashes] == ' ') {
        rendered = ansi::bold(ansi::fgRgb(160, 200, 255, trim(stripped.substr(hashes))));
      } else {
        rendered = renderInlineMarkdown(line);
      }
    } else if (stripped.rfind(">", 0) == 0) {
      rendered = ansi::dim(ansi::fgRgb(170, 180, 205, "▌ " + trim(stripped.substr(1))));
    } else if (stripped.rfind("- ", 0) == 0 || stripped.rfind("* ", 0) == 0) {
      rendered = ansi::fgRgb(150, 190, 255, "• ") + renderInlineMarkdown(trim(stripped.substr(2)));
    } else if (stripped.size() > 3 && std::isdigit(static_cast<unsigned char>(stripped[0]))) {
      size_t dot = stripped.find(". ");
      if (dot != std::string::npos) {
        rendered = ansi::fgRgb(150, 190, 255, stripped.substr(0, dot + 2)) +
                   renderInlineMarkdown(trim(stripped.substr(dot + 2)));
      } else {
        rendered = renderInlineMarkdown(line);
      }
    } else {
      rendered = renderInlineMarkdown(line);
    }

    if (!out.empty()) out += '\n';
    out += rendered;
  }

  return out;
}

} // namespace

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
    styled = ansi::fgRgb(220, 220, 230, renderMarkdownBlock(content));
    break;
  case TranscriptLine::Kind::Thinking:
    styled = ansi::dim(ansi::fgRgb(160, 160, 180, renderMarkdownBlock(content)));
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
