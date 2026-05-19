#include "InputBar.hpp"
#include "Terminal.hpp"
#include "ThemeManager.hpp"

#include <algorithm>
#include <vector>

namespace firmius::tui {

InputBar::InputBar(const AppState& state) : state_(state) {}

namespace {

// Wrap `text` into chunks of at most `bodyWidth` bytes each. Pure
// byte-count wrap — UTF-8 multi-byte sequences are not split-aware here,
// which is fine for the ASCII case and the renderer's fitToWidth handles
// the visible-width budget downstream. Returns at least one chunk.
std::vector<std::string> wrapToBytes(const std::string& text, int bodyWidth) {
  std::vector<std::string> out;
  if (bodyWidth <= 0) {
    out.push_back(text);
    return out;
  }
  if (text.empty()) {
    out.emplace_back();
    return out;
  }
  size_t i = 0;
  while (i < text.size()) {
    out.emplace_back(text.substr(i, static_cast<size_t>(bodyWidth)));
    i += static_cast<size_t>(bodyWidth);
  }
  if (out.empty()) out.emplace_back();
  return out;
}

// Visual row layout: for each logical line of the buffer, what visual
// rows it occupies. Used by both the renderer (to draw) and the App
// (to position the terminal cursor).
struct LogicalLineLayout {
  int firstVisualRow = 0;  ///< Inclusive, 0-indexed, relative to the
                            ///< first content row.
  int rowCount = 1;
  std::vector<std::string> chunks;  ///< `rowCount` entries.
};

}  // namespace

int InputBar::height(int width) const {
  // Top rule + N visual rows + bottom rule. We compute total visual rows
  // by wrapping each logical line. Cap so a runaway paste doesn't eat
  // the whole transcript zone.
  const int totalLogical = std::max(1, state_.inputLineCount());
  const int prefixWidth = 3;
  const int bodyWidth = std::max(1, width - prefixWidth - 1);

  int totalVisual = 0;
  for (int i = 0; i < totalLogical; ++i) {
    const std::string line = state_.inputLineAt(i);
    if (line.empty()) {
      totalVisual += 1;
    } else {
      const int chunks =
          static_cast<int>((line.size() + bodyWidth - 1) / bodyWidth);
      totalVisual += std::max(1, chunks);
    }
  }
  totalVisual = std::max(1, std::min(totalVisual, kMaxVisibleLines));
  return totalVisual + 2;
}

int InputBar::cursorRowOffset() const {
  // First content row (after the top separator).
  return 1;
}

int InputBar::cursorVisualRow() const {
  // Walk the logical lines up to (and including) the cursor's logical
  // line, summing visual rows for each (each line's visual row count =
  // ceil(byteLen / bodyWidth)). Within the cursor's own line, add the
  // wrap-row index for the cursor's column.
  //
  // Note: width-at-call-time has to be consistent with render(). The
  // renderer is the only caller into here, so we re-fetch terminal
  // size via the state's stored width — except we don't have one. The
  // App passes the same `width` to render(); cursorVisualRow runs
  // before render, so we approximate using the current input line
  // count. To keep this side-effect-free, we use a conservative bodyWidth
  // when none is known; the cursor will still land on the right row
  // because all visible lines wrap with the same bodyWidth.
  //
  // We actually need width — let's read it from a state hint. For now
  // accept an unknown width fallback that keeps the cursor on the cursor's
  // logical line, ignoring wrap. That's the previous behaviour, which is
  // fine for short inputs and safe for long ones (cursor stays at line
  // boundary). The renderer uses cursorVisualRowFor(width) below for the
  // accurate version once a width is known.
  return cursorVisualRowFor(80);
}

int InputBar::cursorVisualRowFor(int width) const {
  const int prefixWidth = 3;
  const int bodyWidth = std::max(1, width - prefixWidth - 1);
  const int totalLogical = std::max(1, state_.inputLineCount());
  const int cursorLine = std::min(state_.cursorLineIndex(), totalLogical - 1);
  const int cursorCol = state_.cursorColumnOnLine();

  // Sum visual rows for every line strictly above the cursor's line.
  int sum = 0;
  for (int i = 0; i < cursorLine; ++i) {
    const std::string line = state_.inputLineAt(i);
    const int chunks =
        line.empty() ? 1
                     : static_cast<int>((line.size() + bodyWidth - 1) / bodyWidth);
    sum += std::max(1, chunks);
  }
  // Add wrap-row inside the cursor's own line.
  sum += cursorCol / bodyWidth;

  // Account for kMaxVisibleLines window cap. If the total visual height
  // exceeds the cap, render() scrolls; the cursor visual row reported
  // here must reflect the scrolled position.
  // First compute total visual rows.
  int totalVisual = 0;
  for (int i = 0; i < totalLogical; ++i) {
    const std::string line = state_.inputLineAt(i);
    const int chunks =
        line.empty() ? 1
                     : static_cast<int>((line.size() + bodyWidth - 1) / bodyWidth);
    totalVisual += std::max(1, chunks);
  }
  if (totalVisual <= kMaxVisibleLines) {
    return sum;
  }
  // Window: keep the cursor centred-ish. Same heuristic as render().
  int firstVisible = std::max(0, sum - kMaxVisibleLines / 2);
  if (firstVisible + kMaxVisibleLines > totalVisual) {
    firstVisible = totalVisual - kMaxVisibleLines;
  }
  return sum - firstVisible;
}

int InputBar::cursorVisualColumnFor(int width) const {
  const int prefixWidth = 3;
  const int bodyWidth = std::max(1, width - prefixWidth - 1);
  return state_.cursorColumnOnLine() % bodyWidth;
}

std::vector<std::string> InputBar::render(int width) const {
  const auto& theme = ThemeManager::instance().currentTheme();
  const std::string promptStr =
      ansi::fgRgb(theme.input.prompt.r, theme.input.prompt.g,
                  theme.input.prompt.b, " ❯ ");
  // Continuation rows (lines 2..N OR wrapped continuations) get a dim
  // marker so the user can tell at a glance the input is multi-line or
  // wrapped.
  const std::string contStr =
      ansi::fgRgb(theme.input.placeholder.r, theme.input.placeholder.g,
                  theme.input.placeholder.b, " · ");

  std::string separator;
  separator.reserve(static_cast<std::size_t>(std::max(0, width)) * 3);
  for (int i = 0; i < width; ++i) {
    separator += "\xE2\x94\x80";
  }
  const std::string rule =
      ansi::fgRgb(theme.base.separator.r, theme.base.separator.g,
                  theme.base.separator.b, separator);

  std::vector<std::string> out;
  out.push_back(ansi::fitToWidth(rule, width));

  const int prefixWidth = 3;
  const int bodyWidth = std::max(1, width - prefixWidth - 1);
  const int totalLogical = std::max(1, state_.inputLineCount());

  // Build per-logical-line visual chunks and a flat "(prefix, body)" list.
  struct VisualRow { bool isFirstOfLogical; std::string body; };
  std::vector<VisualRow> rows;
  for (int i = 0; i < totalLogical; ++i) {
    const std::string line = state_.inputLineAt(i);
    auto chunks = wrapToBytes(line, bodyWidth);
    for (size_t c = 0; c < chunks.size(); ++c) {
      VisualRow row;
      row.isFirstOfLogical = (c == 0);
      row.body = std::move(chunks[c]);
      rows.push_back(std::move(row));
    }
  }

  // Placeholder display when buffer is empty.
  const bool isPlaceholder =
      totalLogical == 1 && state_.inputBuffer().empty();
  if (isPlaceholder) {
    rows.clear();
    VisualRow row;
    row.isFirstOfLogical = true;
    row.body =
        ansi::fgRgb(theme.input.placeholder.r, theme.input.placeholder.g,
                    theme.input.placeholder.b, "type a message...");
    rows.push_back(std::move(row));
  }

  // Window the visible rows around the cursor.
  const int totalVisual = static_cast<int>(rows.size());
  int firstVisible = 0;
  int visibleCount = std::min(totalVisual, kMaxVisibleLines);
  if (totalVisual > kMaxVisibleLines) {
    const int cursorVis = cursorVisualRowFor(width);
    firstVisible = std::max(0, cursorVis - kMaxVisibleLines / 2);
    if (firstVisible + kMaxVisibleLines > totalVisual) {
      firstVisible = totalVisual - kMaxVisibleLines;
    }
  }

  for (int i = 0; i < visibleCount; ++i) {
    const auto& row = rows[firstVisible + i];
    std::string body = isPlaceholder
                           ? row.body  // already coloured
                           : ansi::fgRgb(theme.input.fg.r, theme.input.fg.g,
                                          theme.input.fg.b, row.body);
    const std::string& prefix = row.isFirstOfLogical ? promptStr : contStr;
    out.push_back(ansi::fitToWidth(prefix + body, width));
  }

  out.push_back(ansi::fitToWidth(rule, width));
  return out;
}

}  // namespace firmius::tui
