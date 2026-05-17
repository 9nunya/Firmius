#include "StatusBar.hpp"

#include "GlintRenderer.hpp"
#include "Phrases.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"
#include "ThemeManager.hpp"
#include "items/ToolCallItem.hpp"
#include "utils/Icons.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>

namespace firmius::tui2 {

namespace {

using firmius::shared::ICON_AGENT;
using firmius::shared::ICON_CHIP;
using firmius::shared::ICON_CHECK;
using firmius::shared::ICON_CONTEXT;
using firmius::shared::ICON_ERROR;
using firmius::shared::ICON_GEAR;
using firmius::shared::ICON_TODO;
using firmius::shared::ICON_WAIT;
using firmius::shared::PL_LEFT_SEP;
using firmius::shared::TodoStatus;

struct Rgb {
  int r;
  int g;
  int b;
};

struct SegmentSpec {
  Rgb fg;
  Rgb bg;
  std::string text;
};

Rgb rgb(const ThemeRgb& c) { return {c.r, c.g, c.b}; }

std::uint64_t nowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string spinnerGlyph(std::uint64_t now) {
  static constexpr std::array<const char*, 3> kFrames = {"✦", "✧", "⋆"};
  return kFrames[(now / 350ULL) % kFrames.size()];
}

std::string humanize(uint32_t value) {
  if (value >= 1000000) {
    return std::to_string(value / 1000000) + "M";
  }
  if (value >= 1000) {
    return std::to_string(value / 1000) + "k";
  }
  return std::to_string(value);
}

std::string truncateLabel(const std::string& text, int width) {
  if (width <= 0) return "";
  if (ansi::visibleWidth(text) <= width) return text;
  if (width <= 1) return text.substr(0, 1);
  std::string clipped = text;
  while (!clipped.empty() && ansi::visibleWidth(clipped + "…") > width) {
    clipped.pop_back();
  }
  return clipped + "…";
}

std::string renderSegment(const SegmentSpec& current,
                          const SegmentSpec* next = nullptr) {
  std::string body = ansi::bgRgb(
      current.bg.r, current.bg.g, current.bg.b,
      ansi::fgRgb(current.fg.r, current.fg.g, current.fg.b,
                  " " + current.text + " "));
  if (!next) return body;
  return body + ansi::fgRgb(current.bg.r, current.bg.g, current.bg.b,
                            ansi::bgRgb(next->bg.r, next->bg.g, next->bg.b,
                                        PL_LEFT_SEP));
}

std::string renderPowerline(const std::vector<SegmentSpec>& segments) {
  std::string out;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const SegmentSpec* next = i + 1 < segments.size() ? &segments[i + 1] : nullptr;
    out += renderSegment(segments[i], next);
  }
  return out;
}

std::string renderPowerlineToFiller(const std::vector<SegmentSpec>& segments,
                                    Rgb fillerBg) {
  if (segments.empty()) return "";
  std::string out = renderPowerline(segments);
  const auto& tail = segments.back();
  out += ansi::fgRgb(tail.bg.r, tail.bg.g, tail.bg.b,
                     ansi::bgRgb(fillerBg.r, fillerBg.g, fillerBg.b,
                                 PL_LEFT_SEP));
  return out;
}

std::string renderRightPowerlineToFiller(const std::vector<SegmentSpec>& segments,
                                         Rgb fillerBg) {
  if (segments.empty()) return "";
  std::string out = ansi::fgRgb(fillerBg.r, fillerBg.g, fillerBg.b,
                                ansi::bgRgb(segments.front().bg.r,
                                            segments.front().bg.g,
                                            segments.front().bg.b, PL_LEFT_SEP));
  out += renderPowerline(segments);
  return out;
}

std::string fillerSpaces(int n, Rgb bg) {
  if (n <= 0) return "";
  return ansi::bgRgb(bg.r, bg.g, bg.b, std::string(n, ' '));
}

Rgb lerp(Rgb a, Rgb b, float t) {
  const float c = std::clamp(t, 0.0f, 1.0f);
  return {
      static_cast<int>(a.r + (b.r - a.r) * c),
      static_cast<int>(a.g + (b.g - a.g) * c),
      static_cast<int>(a.b + (b.b - a.b) * c),
  };
}

float smoothstep(float t) {
  const float c = std::clamp(t, 0.0f, 1.0f);
  return c * c * (3.0f - 2.0f * c);
}

std::string renderPhraseWipe(std::string_view text,
                             float globalT,
                             bool fadeIn,
                             Rgb bg,
                             Rgb fg) {
  std::string out;
  constexpr float kCharStagger = 0.055f;
  constexpr float kCharFadeWindow = 0.28f;
  const int n = static_cast<int>(text.size());
  const float totalSpan =
      n <= 1 ? kCharFadeWindow
             : (kCharFadeWindow + kCharStagger * static_cast<float>(n - 1));
  const float scaledT = std::clamp(globalT, 0.0f, 1.0f) * totalSpan;
  for (int i = 0; i < n; ++i) {
    const float start = i * kCharStagger;
    const float end = start + kCharFadeWindow;
    const float local = smoothstep((scaledT - start) / (end - start));
    const float alpha = fadeIn ? local : (1.0f - local);
    const auto color = lerp(bg, fg, alpha);
    out += ansi::fgRgb(color.r, color.g, color.b,
                       std::string(1, text[static_cast<std::size_t>(i)]));
  }
  return out;
}

std::string animatedPhrase(const std::vector<std::string>& phrases,
                           std::uint64_t now,
                           bool active,
                           Rgb bg,
                           Rgb fg) {
  if (phrases.empty()) return "Standing by.";
  if (!active || phrases.size() == 1) return phrases.front();

  constexpr std::uint64_t kCycleMs = 5000;
  constexpr std::uint64_t kTransitionMs = 850;
  const std::uint64_t cycleIndex = now / kCycleMs;
  const std::size_t current = static_cast<std::size_t>(cycleIndex % phrases.size());
  const std::size_t next = static_cast<std::size_t>((cycleIndex + 1) % phrases.size());
  const std::uint64_t cyclePos = now % kCycleMs;
  if (cyclePos + kTransitionMs < kCycleMs) {
    return phrases[current];
  }

  const float t = static_cast<float>(cyclePos + kTransitionMs - kCycleMs) /
                  static_cast<float>(kTransitionMs);
  const float fadeOutEnd = 0.56f;
  const float holdEnd = 0.68f;
  if (t < fadeOutEnd) {
    return renderPhraseWipe(phrases[current], t / fadeOutEnd, false, bg, fg);
  }
  if (t < holdEnd) {
    return ansi::fgRgb(bg.r, bg.g, bg.b, phrases[current]);
  }
  return renderPhraseWipe(phrases[next], (t - holdEnd) / (1.0f - holdEnd),
                          true, bg, fg);
}

std::string activitySuffix(const AppState& state) {
  const ToolCallItem* lastTool = nullptr;
  for (const auto& item : state.items()) {
    if (item->type() == "ToolCall") {
      auto* tc = static_cast<const ToolCallItem*>(item.get());
      if (tc->phase() == ToolPhase::Called || tc->phase() == ToolPhase::Preparing) {
        lastTool = tc;
      }
    }
  }
  if (!lastTool) return "";
  return ansi::dim("  ") +
         theme_ansi::warning(ICON_GEAR + std::string(" ")) +
         theme_ansi::foreground(truncateLabel(lastTool->toolName(), 24));
}

std::string todoMarker(TodoStatus status) {
  switch (status) {
  case TodoStatus::InProgress:
    return theme_ansi::warning("■");
  case TodoStatus::Done:
    return theme_ansi::success("✓");
  case TodoStatus::Pending:
  default:
    return theme_ansi::dim("□");
  }
}

std::string todoHeading(int width, const ThemeSpec& theme) {
  std::string line =
      "  " + ansi::fgRgb(theme.base.dim.r, theme.base.dim.g, theme.base.dim.b,
                         ICON_TODO + std::string(" Tasks"));
  return ansi::bgRgb(theme.base.bg.r, theme.base.bg.g, theme.base.bg.b,
                     ansi::fitToWidth(line, width));
}

std::string todoLine(const firmius::shared::TodoItem& item,
                     int width,
                     const ThemeSpec& theme) {
  std::string line = "    " + todoMarker(item.status) + " " +
                     truncateLabel(item.text, width - 10);
  return ansi::bgRgb(theme.base.bg.r, theme.base.bg.g, theme.base.bg.b,
                     ansi::fitToWidth(line, width));
}

ThemeColorGroup statusGroupFor(const ThemeSpec& theme,
                               ConnectionStatus connection,
                               firmius::shared::AgentStatus status) {
  if (connection == ConnectionStatus::Disconnected ||
      status == firmius::shared::AgentStatus::Error ||
      status == firmius::shared::AgentStatus::Cancelled) {
    return theme.statusBar.error.normal;
  }
  if (connection == ConnectionStatus::Connecting) {
    return theme.statusBar.providerWaiting.normal;
  }
  switch (status) {
  case firmius::shared::AgentStatus::Streaming:
    return theme.statusBar.streaming.normal;
  case firmius::shared::AgentStatus::ExecutingTool:
    return theme.statusBar.executingTool.normal;
  case firmius::shared::AgentStatus::ProviderWaiting:
    return theme.statusBar.providerWaiting.normal;
  case firmius::shared::AgentStatus::Compacting:
    return theme.statusBar.compacting.normal;
  case firmius::shared::AgentStatus::Error:
  case firmius::shared::AgentStatus::Cancelled:
    return theme.statusBar.error.normal;
  case firmius::shared::AgentStatus::AwaitingInput:
  case firmius::shared::AgentStatus::Idle:
  default:
    return theme.statusBar.idle.normal;
  }
}

std::string statusIconFor(ConnectionStatus connection,
                          firmius::shared::AgentStatus status) {
  if (connection == ConnectionStatus::Disconnected ||
      status == firmius::shared::AgentStatus::Error ||
      status == firmius::shared::AgentStatus::Cancelled) {
    return ICON_ERROR;
  }
  if (connection == ConnectionStatus::Connecting ||
      status == firmius::shared::AgentStatus::ProviderWaiting ||
      status == firmius::shared::AgentStatus::Compacting) {
    return ICON_WAIT;
  }
  if (status == firmius::shared::AgentStatus::ExecutingTool) {
    return ICON_GEAR;
  }
  if (status == firmius::shared::AgentStatus::Streaming) {
    return ICON_CHECK;
  }
  return ICON_AGENT;
}

} // namespace

StatusBar::StatusBar(const AppState &state) : state_(state) {}

int StatusBar::height(int width) const {
  return liveHeight(width) + hudHeight(width);
}

int StatusBar::liveHeight(int width) const {
  (void)width;
  int rows = 1;
  if (state_.todoVisible()) {
    auto todos = state_.focusedAgentTodos();
    if (!todos.empty()) {
      rows += 1 + std::min<int>(5, static_cast<int>(todos.size()));
    }
  }
  return rows;
}

int StatusBar::hudHeight(int width) const {
  (void)width;
  return 1;
}

std::vector<std::string> StatusBar::renderLiveSection(int width) const {
  const auto& theme = ThemeManager::instance().currentTheme();
  std::vector<std::string> lines = {renderLiveRow(width)};
  if (state_.todoVisible()) {
    auto todos = state_.focusedAgentTodos();
    if (!todos.empty()) {
      lines.push_back(todoHeading(width, theme));
    }
    for (int i = 0; i < std::min<int>(5, static_cast<int>(todos.size())); ++i) {
      lines.push_back(todoLine(todos[static_cast<std::size_t>(i)], width, theme));
    }
  }
  return lines;
}

std::vector<std::string> StatusBar::renderHudSection(int width) const {
  return {renderHudRow(width)};
}

std::vector<std::string> StatusBar::render(int width) const {
  auto lines = renderLiveSection(width);
  auto hud = renderHudSection(width);
  lines.insert(lines.end(), hud.begin(), hud.end());
  return lines;
}

std::string StatusBar::renderLiveRow(int width) const {
  const auto& theme = ThemeManager::instance().currentTheme();
  const auto now = nowMs();
  const auto status = state_.agentStatus();
  const std::string liveMessage = state_.liveMessage();
  const bool busy =
      !liveMessage.empty() ||
      status == firmius::shared::AgentStatus::Streaming ||
      status == firmius::shared::AgentStatus::ExecutingTool ||
      status == firmius::shared::AgentStatus::ProviderWaiting ||
      status == firmius::shared::AgentStatus::Compacting;

  std::string mode = "idle";
  if (!liveMessage.empty()) {
    mode = "waiting";
  } else if (status == firmius::shared::AgentStatus::Streaming) {
    mode = "thinking";
  } else if (status == firmius::shared::AgentStatus::ExecutingTool) {
    mode = "editing";
  } else if (status == firmius::shared::AgentStatus::ProviderWaiting ||
             status == firmius::shared::AgentStatus::Compacting) {
    mode = "waiting";
  } else if (status != firmius::shared::AgentStatus::Idle) {
    mode = "working";
  }

  std::string phrase =
      liveMessage.empty()
          ? animatedPhrase(livePhrasesForMode(mode), now, busy, rgb(theme.base.bg),
                           rgb(theme.base.highlight))
          : liveMessage;

  if (busy && liveMessage.empty() && phrase.find("\x1b[") == std::string::npos) {
    GlintConfig cfg;
    cfg.gradient = {
        {theme.base.highlight.r, theme.base.highlight.g, theme.base.highlight.b},
        {theme.base.fg.r, theme.base.fg.g, theme.base.fg.b},
        {theme.base.highlight.r, theme.base.highlight.g, theme.base.highlight.b},
    };
    phrase = GlintRenderer::apply(phrase, cfg, now, true);
  } else if (!busy) {
    phrase = ansi::dim(phrase);
  }

  std::string spinner =
      busy ? ansi::fgRgb(theme.base.highlight.r, theme.base.highlight.g,
                         theme.base.highlight.b,
                         " " + spinnerGlyph(now) + " ")
           : ansi::dim(" • ");

  std::string line = spinner + ansi::bold(phrase) + activitySuffix(state_);
  return ansi::bgRgb(theme.base.bg.r, theme.base.bg.g, theme.base.bg.b,
                     ansi::fitToWidth(line, width));
}

std::string StatusBar::renderHudRow(int width) const {
  const auto& theme = ThemeManager::instance().currentTheme();
  const auto statusGroup =
      statusGroupFor(theme, state_.connectionStatus(), state_.agentStatus());
  const std::string stateIcon =
      statusIconFor(state_.connectionStatus(), state_.agentStatus());

  std::string purpose = state_.agentPurpose().empty() ? "lead" : state_.agentPurpose();
  std::string title = state_.threadTitle().empty() ? "New Thread" : state_.threadTitle();
  std::string model = state_.modelLabel().empty() ? "no model" : state_.modelLabel();

  auto ctx = state_.agentContextUsage();
  if (ctx.windowTokens == 0) {
    auto label = state_.modelLabel();
    if (label.find("gpt-5.4") != std::string::npos) {
      ctx.windowTokens = 262000;
    } else if (label.find("mimo-v2.5-pro") != std::string::npos) {
      ctx.windowTokens = 1048000;
    }
  }
  const uint32_t window = std::max<uint32_t>(ctx.windowTokens, 1);
  const uint32_t used = ctx.usedTokens > 0 ? ctx.usedTokens : ctx.sentTokens;
  const std::string ctxLabel = humanize(used) + "/" + humanize(window);

  std::vector<SegmentSpec> left = {
      {rgb(statusGroup.fg), rgb(statusGroup.bg), stateIcon},
      {rgb(theme.statusBar.agentFg), rgb(theme.statusBar.agentBg), purpose},
      {rgb(theme.statusBar.pillFg), rgb(theme.statusBar.pillBg),
       truncateLabel(title, 24)},
      {rgb(theme.base.bg), rgb(theme.base.highlight), truncateLabel(model, 22)},
  };

  std::vector<SegmentSpec> right = {
      {rgb(theme.statusBar.context.icon), rgb(theme.statusBar.context.bg),
       ICON_CONTEXT + std::string(" ") + ctxLabel},
      {rgb(theme.base.fg), rgb(theme.agentStrip.pills.stateBg),
       ICON_CHIP + std::string(" ") +
           std::to_string(state_.queuedMessageCount())},
      {rgb(theme.base.fg), rgb(theme.agentStrip.pills.contextBg),
       state_.todoVisible() ? ICON_TODO + std::string(" on")
                            : ICON_TODO + std::string(" off")},
  };

  // Build left and right bars independently. Do NOT wrap the whole row in an
  // outer bgRgb() — every inner segment closes with \x1b[49m which would
  // clobber an outer bg. Instead, fill the middle gap and any trailing space
  // with explicit fillerBg-colored spaces so the entire row reads as one
  // continuous filler bg.
  const Rgb fillerBg = rgb(theme.statusBar.fillerBg);
  std::string leftBar = renderPowerlineToFiller(left, fillerBg);
  std::string rightBar = renderRightPowerlineToFiller(right, fillerBg);

  const int leftVis = ansi::visibleWidth(leftBar);
  const int rightVis = ansi::visibleWidth(rightBar);

  // If the combined bars don't fit, prefer keeping the right side intact and
  // truncate the left (rare in practice — segments are already truncated).
  // We can't easily truncate ANSI-wrapped strings here, so just emit them
  // as-is and let the terminal clip; the gap math below clamps to 0.
  const int gap = std::max(0, width - leftVis - rightVis);
  // gap math guarantees: leftVis + gap + rightVis == max(width, leftVis+rightVis)
  // — i.e. the line reaches the right edge whenever the bars fit. Overflow
  // (rendered > width) is left to the terminal to clip.
  return leftBar + fillerSpaces(gap, fillerBg) + rightBar;
}

} // namespace firmius::tui2
