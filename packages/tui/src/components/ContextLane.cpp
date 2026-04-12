#include "components/ContextLane.hpp"

#include "ThemeManager.hpp"
#include "utils/Icons.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ftxui/dom/elements.hpp>
#include <iomanip>
#include <sstream>

namespace firmius::tui {

namespace {

std::string shorten(std::string text, std::size_t limit) {
  if (text.size() <= limit) {
    return text;
  }
  if (limit <= 3) {
    return text.substr(0, limit);
  }
  return text.substr(0, limit - 1) + "…";
}

std::string repeatGlyph(const std::string &glyph, int count) {
  std::string out;
  for (int i = 0; i < std::max(0, count); ++i) {
    out += glyph;
  }
  return out;
}

ftxui::Element pill(const std::string &icon, const std::string &label,
                    ftxui::Color fg, ftxui::Color bg) {
  return ftxui::text(" " + icon + " " + label + " ") | ftxui::bold |
         ftxui::color(fg) | ftxui::bgcolor(bg);
}

ftxui::Color contextColor(const Theme &theme, float ratio) {
  const float clamped = std::clamp(ratio, 0.0f, 1.0f);
  if (clamped > 0.85f) {
    return theme.status_bar.context.high;
  }
  if (clamped > 0.60f) {
    return theme.status_bar.context.medium;
  }
  return theme.status_bar.context.low;
}

ftxui::Element contextMeter(const Theme &theme, float ratio) {
  constexpr int kMeterUnits = 14;
  const float clamped = std::clamp(ratio, 0.0f, 1.0f);
  const int filled =
      std::clamp(static_cast<int>(std::round(clamped * kMeterUnits)), 0,
                 kMeterUnits);
  const int empty = kMeterUnits - filled;
  const auto color = contextColor(theme, clamped);

  return ftxui::hbox({
      ftxui::text(repeatGlyph("█", filled)) | ftxui::bold |
          ftxui::color(color),
      ftxui::text(repeatGlyph("░", empty)) |
          ftxui::color(theme.base.border),
  });
}

std::string formatCompactCount(std::uint32_t value) {
  std::ostringstream out;
  if (value >= 1000000) {
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(value) / 1000000.0);
    std::string text = out.str();
    if (text.size() > 2 && text.substr(text.size() - 2) == ".0") {
      text.erase(text.size() - 2);
    }
    return text + "M";
  }
  if (value >= 1000) {
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(value) / 1000.0);
    std::string text = out.str();
    if (text.size() > 2 && text.substr(text.size() - 2) == ".0") {
      text.erase(text.size() - 2);
    }
    return text + "k";
  }
  return std::to_string(value);
}

std::string rollingRailGlyphs(float occupancyRatio, float bufferRatio,
                             float targetRatio, float emergencyRatio) {
  constexpr int kUnits = 22;
  std::vector<std::string> rail(static_cast<std::size_t>(kUnits), "░");
  const int filled = std::clamp(
      static_cast<int>(std::round(std::clamp(occupancyRatio, 0.0f, 1.0f) *
                                  static_cast<float>(kUnits))),
      0, kUnits);
  for (int i = 0; i < filled; ++i) {
    rail[static_cast<std::size_t>(i)] = "█";
  }
  auto mark = [&](float ratio, const std::string &glyph) {
    const int idx = std::clamp(
        static_cast<int>(std::round(std::clamp(ratio, 0.0f, 1.0f) *
                                    static_cast<float>(kUnits - 1))),
        0, kUnits - 1);
    rail[static_cast<std::size_t>(idx)] = glyph;
  };
  mark(bufferRatio, "B");
  mark(targetRatio, "T");
  mark(emergencyRatio, "E");
  std::string out;
  out.reserve(static_cast<std::size_t>(kUnits * 3));
  for (const auto &glyph : rail) {
    out += glyph;
  }
  return out;
}

} // namespace

class ContextLaneComponentBase : public ftxui::ComponentBase {
public:
  explicit ContextLaneComponentBase(std::shared_ptr<ContextLaneModel> model)
      : model_(std::move(model)) {}

  ftxui::Element OnRender() override {
    if (!model_ || !model_->visible) {
      return ftxui::text("");
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    return RenderBody() | ftxui::bgcolor(theme.agent_strip.bg) | ftxui::xflex;
  }

private:
  ftxui::Element RenderBody() {
    if (!model_ || !model_->visible) {
      return ftxui::text("");
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    int term_width = ftxui::Terminal::Size().dimx;
    bool compact_mode = term_width <= 110;
    bool ultra_compact = term_width < 70;
    ftxui::Elements rows;
    ftxui::Elements chips;
    if (!model_->model_label.empty() && !ultra_compact) {
      chips.push_back(
          pill(firmius::shared::ICON_CHIP, shorten(model_->model_label, compact_mode ? 20 : 40),
               theme.agent_strip.pills.model_fg,
               theme.agent_strip.pills.model_bg));
    }
    if (!model_->account_label.empty()) {
      if (!chips.empty()) chips.push_back(ftxui::text(" "));
      chips.push_back(
          pill(firmius::shared::ICON_PERSONA, shorten(model_->account_label, ultra_compact ? 12 : compact_mode ? 20 : 40),
               theme.agent_strip.pills.slug_fg,
               theme.agent_strip.pills.slug_bg));
    }
    if (!model_->quota_label.empty()) {
      if (!chips.empty()) chips.push_back(ftxui::text(" "));
      chips.push_back(
          pill("󰾆", shorten(model_->quota_label, ultra_compact ? 12 : compact_mode ? 20 : 40),
               theme.agent_strip.pills.state_fg,
               theme.agent_strip.pills.state_bg));
    }
    if (!chips.empty()) {
      rows.push_back(ftxui::hbox(std::move(chips)) | ftxui::xflex);
    }

    const float ratio = std::clamp(model_->context_ratio, 0.0f, 1.0f);
    char pct_buf[16];
    std::snprintf(pct_buf, sizeof(pct_buf), "%.0f%%", ratio * 100.0f);
    const auto ctx_color = contextColor(theme, ratio);
    std::string metrics_text = model_->usage_label;
    if (!model_->cost_label.empty()) {
      if (!metrics_text.empty()) {
        metrics_text += " · ";
      }
      metrics_text += model_->cost_label;
    }

    std::string bucket_text;
    for (std::size_t i = 0; i < model_->bucket_labels.size(); ++i) {
      if (i > 0) {
        bucket_text += " · ";
      }
      bucket_text += model_->bucket_labels[i];
    }

    std::string trailing_text;
    if (!metrics_text.empty()) {
      trailing_text += " " + firmius::shared::ICON_METRICS + " " +
                       shorten(metrics_text, ultra_compact ? 15 : compact_mode ? 25 : 50);
    }
    if (!bucket_text.empty()) {
      if (!trailing_text.empty()) {
        trailing_text += "   ";
      }
      trailing_text += " " + firmius::shared::ICON_BOOK + " " +
                       shorten(bucket_text, ultra_compact ? 10 : compact_mode ? 20 : 40);
    }
    if (ultra_compact) {
      rows.push_back(
          ftxui::hbox({
              ftxui::text(" " + firmius::shared::ICON_CONTEXT + " ") |
                  ftxui::color(theme.status_bar.context.icon),
              contextMeter(theme, ratio) | ftxui::xflex,
              ftxui::text(" " + std::string(pct_buf)) | ftxui::bold |
                  ftxui::color(ctx_color),
          }) |
          ftxui::xflex);
    } else {
      rows.push_back(
          ftxui::hbox({
              ftxui::text(" " + firmius::shared::ICON_CONTEXT + " ") |
                  ftxui::color(theme.status_bar.context.icon),
              contextMeter(theme, ratio) | ftxui::xflex,
              ftxui::text(" " + std::string(pct_buf)) | ftxui::bold |
                  ftxui::color(ctx_color),
              model_->context_label.empty()
                  ? ftxui::text("")
                  : ftxui::text("  " + shorten(model_->context_label, compact_mode ? 25 : 40)) |
                        ftxui::color(theme.base.dim),
              trailing_text.empty()
                  ? ftxui::text("")
                  : ftxui::text("  " + trailing_text) |
                        ftxui::color(theme.base.dim),
          }) |
          ftxui::xflex);
    }

    if (model_->rolling_memory.enabled) {
      const auto &rolling = model_->rolling_memory;
      ftxui::Elements rolling_chips;
      rolling_chips.push_back(
          pill("🧠", shorten(rolling.mode_label.empty() ? std::string("rolling")
                                                      : rolling.mode_label,
                             compact_mode ? 18 : 28),
               theme.agent_strip.pills.state_fg,
               theme.agent_strip.pills.state_bg));
      if (!rolling.preset_label.empty()) {
        rolling_chips.push_back(ftxui::text(" "));
        rolling_chips.push_back(
            pill("⚙", shorten(rolling.preset_label, compact_mode ? 14 : 20),
                 theme.agent_strip.pills.slug_fg,
                 theme.agent_strip.pills.slug_bg));
      }
      if (!rolling.model_label.empty() && !ultra_compact) {
        rolling_chips.push_back(ftxui::text(" "));
        rolling_chips.push_back(
            pill(firmius::shared::ICON_CHIP,
                 shorten(rolling.model_label, compact_mode ? 22 : 36),
                 theme.agent_strip.pills.model_fg,
                 theme.agent_strip.pills.model_bg));
      }
      rows.push_back(ftxui::hbox(std::move(rolling_chips)) | ftxui::xflex);

      ftxui::Elements activity_chips;
      activity_chips.push_back(
          pill("◍", "obs " + std::to_string(rolling.active_observations),
               theme.agent_strip.pills.slug_fg,
               theme.agent_strip.pills.slug_bg));
      activity_chips.push_back(ftxui::text(" "));
      activity_chips.push_back(
          pill("◔", "refl " + std::to_string(rolling.active_reflections),
               theme.agent_strip.pills.slug_fg,
               theme.agent_strip.pills.slug_bg));
      activity_chips.push_back(ftxui::text(" "));
      activity_chips.push_back(
          pill("◌", "buf " + std::to_string(rolling.buffered_observations),
               theme.agent_strip.pills.slug_fg,
               theme.agent_strip.pills.slug_bg));
      if (rolling.observation_in_flight || rolling.reflection_in_flight) {
        activity_chips.push_back(ftxui::text(" "));
        std::string inflight = "active";
        if (rolling.observation_in_flight && rolling.reflection_in_flight) {
          inflight = "obs+refl";
        } else if (rolling.observation_in_flight) {
          inflight = "obs";
        } else if (rolling.reflection_in_flight) {
          inflight = "refl";
        }
        activity_chips.push_back(
            pill("⟳", inflight + " in-flight",
                 theme.status_bar.compacting.normal.fg,
                 theme.status_bar.compacting.normal.bg));
      }
      rows.push_back(ftxui::hbox(std::move(activity_chips)) | ftxui::xflex);

      const float occupancy = std::clamp(rolling.context_occupancy_ratio, 0.0f, 1.0f);
      char occBuf[8];
      std::snprintf(occBuf, sizeof(occBuf), "%.0f%%", occupancy * 100.0f);
      const auto rollingColor = contextColor(theme, occupancy);
      rows.push_back(ftxui::hbox({
                        ftxui::text("  rail ") | ftxui::color(theme.base.dim),
                        ftxui::text(rollingRailGlyphs(
                                        occupancy, rolling.buffer_threshold_ratio,
                                        rolling.target_threshold_ratio,
                                        rolling.emergency_threshold_ratio)) |
                            ftxui::bold | ftxui::color(rollingColor),
                        ftxui::text(" " + std::string(occBuf)) | ftxui::bold |
                            ftxui::color(rollingColor),
                        ftxui::text("  B" +
                                    std::to_string(static_cast<int>(std::round(
                                        rolling.buffer_threshold_ratio * 100.0f))) +
                                    " T" +
                                    std::to_string(static_cast<int>(std::round(
                                        rolling.target_threshold_ratio * 100.0f))) +
                                    " E" +
                                    std::to_string(static_cast<int>(std::round(
                                        rolling.emergency_threshold_ratio *
                                        100.0f)))) |
                            ftxui::color(theme.base.dim),
                      }) |
                      ftxui::xflex);

      std::string compressionText =
          "src " + formatCompactCount(rolling.source_tokens) +
          " · sum " + formatCompactCount(rolling.summary_tokens) +
          " · saved " + formatCompactCount(rolling.saved_tokens);
      if (rolling.source_tokens > 0 && rolling.saved_tokens > 0) {
        const int savedPct = static_cast<int>(std::round(
            (static_cast<double>(rolling.saved_tokens) /
             static_cast<double>(rolling.source_tokens)) *
            100.0));
        compressionText += " (" + std::to_string(savedPct) + "%)";
      }
      compressionText += " · tail " + formatCompactCount(rolling.retained_tail_tokens);
      rows.push_back(ftxui::text("  " + shorten(compressionText, compact_mode ? 72 : 104)) |
                     ftxui::color(theme.base.dim));
    } else if (!model_->memory_labels.empty()) {
      for (const auto &label : model_->memory_labels) {
        rows.push_back(ftxui::text("  🧠 " +
                                    shorten(label, compact_mode ? 64 : 96)) |
                       ftxui::color(theme.base.dim));
      }
    }

    return ftxui::vbox(std::move(rows)) | ftxui::xflex;
  }

  std::shared_ptr<ContextLaneModel> model_;
};

ftxui::Component ContextLane(const std::shared_ptr<ContextLaneModel> &model) {
  return std::make_shared<ContextLaneComponentBase>(model);
}

} // namespace firmius::tui
