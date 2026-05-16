#include "items/StreamingItems.hpp"
#include "Terminal.hpp"

#include <algorithm>
#include <sstream>

namespace firmius::tui2 {

namespace {

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
    out.push_back(text[i++]);
  }
  return out;
}

std::string renderMarkdownBlock(const std::string& text, bool dimmed) {
  std::istringstream stream(text);
  std::string line;
  std::string out;
  bool inFence = false;

  while (std::getline(stream, line)) {
    std::string rendered;
    if (line.rfind("```", 0) == 0) {
      inFence = !inFence;
      rendered = ansi::dim(ansi::fgRgb(150, 150, 170, line));
    } else if (inFence) {
      rendered = ansi::bgRgb(25, 25, 32, ansi::fgRgb(210, 210, 220, line));
    } else if (!line.empty() && line[0] == '#') {
      size_t hashes = 0;
      while (hashes < line.size() && line[hashes] == '#') ++hashes;
      if (hashes < line.size() && line[hashes] == ' ') {
        rendered = ansi::bold(ansi::fgRgb(160, 200, 255, line.substr(hashes + 1)));
      } else {
        rendered = renderInlineMarkdown(line);
      }
    } else if (line.rfind("> ", 0) == 0) {
      rendered = ansi::dim(ansi::fgRgb(170, 180, 205, "\xe2\x96\x8c " + line.substr(2)));
    } else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
      rendered = ansi::fgRgb(150, 190, 255, "\xe2\x80\xa2 ") + renderInlineMarkdown(line.substr(2));
    } else {
      rendered = renderInlineMarkdown(line);
    }

    if (dimmed) {
      rendered = ansi::dim(ansi::fgRgb(160, 160, 180, rendered));
    }

    if (!out.empty()) out += '\n';
    out += rendered;
  }
  return out;
}

// Measure visual width of a string, ignoring ANSI escape sequences.
int visualWidth(const std::string& s) {
  int w = 0;
  for (size_t i = 0; i < s.size(); ) {
    if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
      i += 2;
      while (i < s.size() && s[i] != 'm') ++i;
      if (i < s.size()) ++i;
    } else {
      ++w;
      ++i;
    }
  }
  return w;
}

// Wrap a single rendered line to fit within maxWidth visual columns.
// Splits at word boundaries when possible, hard-splits if no space found.
// Preserves ANSI codes across splits.
// stylePrefix is reapplied to each continuation line (e.g. dim ANSI codes)
// so that styling persists across wrapped lines.
std::vector<std::string> wrapLine(const std::string& line, int maxWidth,
                                   const std::string& stylePrefix = "") {
  if (maxWidth <= 0) return {line};

  std::vector<std::string> result;
  std::string current;
  int currentWidth = 0;
  size_t lastSpacePos = 0;
  int lastSpaceWidth = 0;

  for (size_t i = 0; i < line.size(); ) {
    if (line[i] == '\033' && i + 1 < line.size() && line[i + 1] == '[') {
      size_t start = i;
      i += 2;
      while (i < line.size() && line[i] != 'm') ++i;
      if (i < line.size()) ++i;
      current += line.substr(start, i - start);
      continue;
    }

    if (line[i] == ' ') {
      lastSpacePos = current.size();
      lastSpaceWidth = currentWidth;
    }

    current += line[i];
    ++currentWidth;
    ++i;

    if (currentWidth >= maxWidth) {
      if (lastSpacePos > 0 && lastSpaceWidth > 0) {
        result.push_back(current.substr(0, lastSpacePos));
        current = current.substr(lastSpacePos + 1);
        currentWidth = visualWidth(current);
      } else {
        result.push_back(current);
        current.clear();
        currentWidth = 0;
      }
      // Re-apply style prefix so dim/etc. persists across wrapped lines.
      // Without this, the terminal resets styling at line boundaries and
      // continuation lines lose the dim effect.
      if (!stylePrefix.empty()) {
        current = stylePrefix + current;
        currentWidth = visualWidth(current);
      }
      lastSpacePos = 0;
      lastSpaceWidth = 0;
    }
  }
  if (!current.empty()) result.push_back(current);
  if (result.empty()) result.push_back("");
  return result;
}

std::vector<std::string> renderStreamingBlock(const std::string& text, int width,
                                               const std::string& prefix, bool dimmed) {
  std::vector<std::string> lines;
  std::string rendered = renderMarkdownBlock(text, dimmed);
  std::istringstream stream(rendered);
  std::string line;
  int wrapWidth = width - static_cast<int>(prefix.size());

  // For dimmed text, build a style prefix with dim ANSI codes that gets
  // reapplied to each continuation line after wrapping.
  std::string stylePrefix;
  if (dimmed) {
    stylePrefix = "\x1b[2m\x1b[38;2;160;160;180m";
  }

  while (std::getline(stream, line)) {
    if (wrapWidth > 0 && visualWidth(line) > wrapWidth) {
      auto wrapped = wrapLine(line, wrapWidth, stylePrefix);
      for (auto& wl : wrapped) {
        lines.push_back(prefix + wl);
      }
    } else {
      lines.push_back(prefix + line);
    }
  }
  if (lines.empty()) lines.push_back(prefix);
  return lines;
}

int countBlockLines(const std::string& text, int width, int prefixLen) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  int count = 0;
  while (std::getline(stream, line)) {
    int effectiveWidth = width - prefixLen;
    if (effectiveWidth <= 0) effectiveWidth = 1;
    count += std::max(1, (static_cast<int>(line.size()) + effectiveWidth - 1) / effectiveWidth);
  }
  return std::max(1, count);
}

} // namespace

// ── AgentTextItem ──

void AgentTextItem::appendDelta(const std::string& delta) {
  accumulated_ += delta;
  touch();
}

void AgentTextItem::finalize() {
  finalized_ = true;
  touch();
}

std::vector<std::string> AgentTextItem::render(int width) const {
  return renderStreamingBlock(accumulated_, width, "", false);
}

int AgentTextItem::rowCount(int width) const {
  return countBlockLines(accumulated_, width, 0);
}

// ── AgentThinkingItem ──

void AgentThinkingItem::appendDelta(const std::string& delta) {
  accumulated_ += delta;
  touch();
}

void AgentThinkingItem::finalize() {
  finalized_ = true;
  touch();
}

std::vector<std::string> AgentThinkingItem::render(int width) const {
  return renderStreamingBlock(accumulated_, width, "", true);
}

int AgentThinkingItem::rowCount(int width) const {
  return countBlockLines(accumulated_, width, 0);
}

} // namespace firmius::tui2
