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

std::string repeatGlyph(const std::string &glyph, int count) {
  std::string out;
  for (int i = 0; i < std::max(0, count); ++i) {
    out += glyph;
  }
  return out;
}

std::unordered_map<std::string, ftxui::Color> getBucketColors(const Theme &theme);

ftxui::Element pill(const std::string &icon, const std::string &label,
                    ftxui::Color fg, ftxui::Color bg) {
  return ftxui::text(" " + icon + " " + label + " ") | ftxui::bold |
         ftxui::color(fg) | ftxui::bgcolor(bg);
}

std::string truncateText(const std::string &text, std::size_t max_len) {
  if (max_len == 0 || text.size() <= max_len) {
    return text;
  }
  if (max_len <= 1) {
    return "…";
  }
  return text.substr(0, max_len - 1) + "…";
}

ftxui::Element detailLine(const std::string &label, const std::string &value,
                          ftxui::Color color, std::size_t max_len = 0) {
  const std::string visible = max_len > 0 ? truncateText(value, max_len) : value;
  return ftxui::text("  " + label + visible) | ftxui::color(color);
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

ftxui::Element unifiedContextBar(const Theme &theme, const ContextLaneModel &model) {
  const int barWidth = ftxui::Terminal::Size().dimx - 30;
  const float totalRatio = std::clamp(model.context_ratio, 0.0f, 1.0f);
  const int totalFilledWidth = static_cast<int>(std::round(totalRatio * barWidth));
  
  auto bucketColors = getBucketColors(theme);
  
  ftxui::Elements segments;
  int allocated = 0;
  
  // Calculate total bucket tokens for relative scaling
  uint32_t totalBucketTokens = 0;
  for (const auto &bucket : model.context_buckets) {
    totalBucketTokens += bucket.tokens;
  }
  
  // Render each context bucket in chronological order (left to right)
  for (const auto &bucket : model.context_buckets) {
    if (bucket.tokens == 0) continue;
    
    // Scale bucket width proportionally to its share of the total filled space
    const float bucketShare = totalBucketTokens > 0 
        ? static_cast<float>(bucket.tokens) / totalBucketTokens
        : 0.0f;
    const int width = std::max(1, static_cast<int>(std::round(bucketShare * totalFilledWidth)));
    if (allocated + width > totalFilledWidth) break;
    
    auto colorIt = bucketColors.find(bucket.label);
    ftxui::Color color = colorIt != bucketColors.end() 
        ? colorIt->second 
        : theme.status_bar.context.low;
    
    segments.push_back(ftxui::text(repeatGlyph("█", width)) 
        | ftxui::color(color) | ftxui::bold);
    
    allocated += width;
  }
  
  // Add empty space for remaining context window
  const int emptyWidth = barWidth - totalFilledWidth;
  if (emptyWidth > 0) {
    segments.push_back(ftxui::text(repeatGlyph("░", emptyWidth)) 
        | ftxui::color(theme.base.border));
  }
  
  return ftxui::hbox(std::move(segments));
}

std::unordered_map<std::string, ftxui::Color> getBucketColors(const Theme &theme) {
  return {
    {"system", theme.status_bar.context.high},        // Fixed system prompt (leftmost)
    {"skills", theme.tool_blocks.generic_icon},       // Loaded skills
    {"files", theme.tool_blocks.specific.file_read.fg}, // Watched files
    {"plan", theme.syntax.constant},                  // Live work state
    {"memory", theme.status_bar.context.medium},      // Rolling observations
    {"history", theme.chat.user_prefix},              // Conversation history
    {"tools", theme.tool_blocks.specific.terminal.fg},// Tool schemas & history
    {"tool_history", theme.tool_blocks.specific.terminal.fg},
    {"tool_schemas", theme.tool_blocks.specific.file_edit.fg},
    {"user", theme.chat.user_prefix},                 // Latest user message
    {"assistant", theme.chat.agent_prefix},           // Assistant response
    {"recall", theme.syntax.string},                  // Retrieval results
    {"images", theme.syntax.comment},                 // Image attachments
    {"rolling", theme.status_bar.context.medium},
    {"observations", theme.status_bar.context.medium},
    {"messages", theme.chat.user_prefix},
  };
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
    ftxui::Elements rows;
    ftxui::Elements chips;
    
    // Model/account chips (NO TRUNCATION)
    if (!model_->model_label.empty()) {
      chips.push_back(
          pill(firmius::shared::ICON_CHIP, model_->model_label,
               theme.agent_strip.pills.model_fg,
               theme.agent_strip.pills.model_bg));
    }
    if (!model_->account_label.empty()) {
      if (!chips.empty()) chips.push_back(ftxui::text(" "));
      chips.push_back(
          pill(firmius::shared::ICON_PERSONA, model_->account_label,
               theme.agent_strip.pills.slug_fg,
               theme.agent_strip.pills.slug_bg));
    }
    if (!model_->quota_label.empty()) {
      if (!chips.empty()) chips.push_back(ftxui::text(" "));
      chips.push_back(
          pill("󰾆", model_->quota_label,
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

    ftxui::Elements context_header;
    context_header.push_back(
        ftxui::text(" " + firmius::shared::ICON_CONTEXT + " ") |
            ftxui::color(theme.status_bar.context.icon));
    context_header.push_back(unifiedContextBar(theme, *model_) | ftxui::xflex);
    context_header.push_back(
        ftxui::text(" " + std::string(pct_buf)) | ftxui::bold |
            ftxui::color(ctx_color));
    if (!model_->context_label.empty()) {
      context_header.push_back(
          ftxui::text("  " + model_->context_label) | ftxui::color(theme.base.dim));
    }
    rows.push_back(ftxui::hbox(std::move(context_header)) | ftxui::xflex);

    // Metrics and bucket labels
    ftxui::Elements metric_row;
    if (!model_->usage_label.empty()) {
      metric_row.push_back(ftxui::text("  " + firmius::shared::ICON_METRICS + " " + model_->usage_label) |
                          ftxui::color(theme.base.dim));
    }
    if (!model_->cost_label.empty()) {
      if (!metric_row.empty()) metric_row.push_back(ftxui::text("  "));
      metric_row.push_back(ftxui::text(model_->cost_label) | ftxui::color(theme.base.dim));
    }
    if (!model_->bucket_labels.empty()) {
      if (!metric_row.empty()) metric_row.push_back(ftxui::text("  "));
      metric_row.push_back(ftxui::text(firmius::shared::ICON_BOOK + " ") | ftxui::color(theme.base.dim));
      for (std::size_t i = 0; i < model_->bucket_labels.size(); ++i) {
        if (i > 0) metric_row.push_back(ftxui::text(" · ") | ftxui::color(theme.base.dim));
        metric_row.push_back(ftxui::text(model_->bucket_labels[i]) | ftxui::color(theme.base.dim));
      }
    }
    if (!metric_row.empty()) {
      rows.push_back(ftxui::hbox(std::move(metric_row)) | ftxui::xflex);
    }

    if (model_->rolling_memory.enabled) {
      const auto &rolling = model_->rolling_memory;
      rows.push_back(ftxui::text("  Rolling memory") | ftxui::bold |
                     ftxui::color(theme.base.fg));

      ftxui::Elements rolling_summary;
      rolling_summary.push_back(
          pill(firmius::shared::ICON_BRAIN,
               rolling.mode_label.empty() ? std::string("rolling")
                                          : rolling.mode_label,
               theme.agent_strip.pills.state_fg,
               theme.agent_strip.pills.state_bg));
      if (!rolling.preset_label.empty()) {
        rolling_summary.push_back(ftxui::text(" "));
        rolling_summary.push_back(
            pill(firmius::shared::ICON_GEAR, rolling.preset_label,
                 theme.agent_strip.pills.slug_fg,
                 theme.agent_strip.pills.slug_bg));
      }
      if (!rolling.model_label.empty()) {
        rolling_summary.push_back(ftxui::text(" "));
        rolling_summary.push_back(
            pill(firmius::shared::ICON_CHIP, rolling.model_label,
                 theme.agent_strip.pills.model_fg,
                 theme.agent_strip.pills.model_bg));
      }
      rows.push_back(ftxui::hbox(std::move(rolling_summary)) | ftxui::xflex);

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
      rows.push_back(
          ftxui::hbox({ftxui::text("  Activity") | ftxui::bold |
                          ftxui::color(theme.base.dim),
                      ftxui::text(" "),
                      ftxui::hbox(std::move(activity_chips))}) |
          ftxui::xflex);

      ftxui::Elements bucket_line;
      bucket_line.push_back(ftxui::text("  Buckets") | ftxui::bold |
                            ftxui::color(theme.base.dim));
      bucket_line.push_back(ftxui::text(": ") | ftxui::color(theme.base.dim));
      auto bucketColors = getBucketColors(theme);
      int bucketCount = 0;
      for (const auto &bucket : model_->context_buckets) {
        if (bucket.tokens == 0 || bucketCount >= 6) continue;
        if (bucketCount > 0) {
          bucket_line.push_back(ftxui::text("   ") | ftxui::color(theme.base.dim));
        }
        auto colorIt = bucketColors.find(bucket.label);
        ftxui::Color color = colorIt != bucketColors.end() ? colorIt->second
                                                           : theme.status_bar.context.low;
        bucket_line.push_back(ftxui::text("█") | ftxui::color(color));
        bucket_line.push_back(ftxui::text(" " + bucket.label) |
                              ftxui::color(theme.base.dim));
        ++bucketCount;
      }
      if (bucketCount > 0) {
        rows.push_back(ftxui::hbox(std::move(bucket_line)) | ftxui::xflex);
      }

      ftxui::Elements thresholds_row;
      thresholds_row.push_back(ftxui::text("  Thresholds") | ftxui::bold |
                               ftxui::color(theme.base.dim));
      thresholds_row.push_back(
          ftxui::text(" buffer " + formatCompactCount(rolling.buffer_threshold_tokens)) |
          ftxui::color(theme.base.dim));
      thresholds_row.push_back(
          ftxui::text(" · target " + formatCompactCount(rolling.target_threshold_tokens)) |
          ftxui::color(theme.base.dim));
      thresholds_row.push_back(
          ftxui::text(" · emergency " +
                     formatCompactCount(rolling.emergency_threshold_tokens)) |
          ftxui::color(theme.base.dim));
      rows.push_back(ftxui::hbox(std::move(thresholds_row)) | ftxui::xflex);

      std::string compressionText =
          "  Compression src " + formatCompactCount(rolling.source_tokens) +
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
      rows.push_back(ftxui::text(compressionText) | ftxui::color(theme.base.dim));

      std::string bridgeText =
          "  Bridge anchors " + std::to_string(rolling.canonical_anchor_count) +
          " · bridges " + std::to_string(rolling.bridge_packet_count);
      if (!rolling.latest_bridge_id.empty()) {
        bridgeText += " · latest " + rolling.latest_bridge_id;
      }
      rows.push_back(ftxui::text(bridgeText) | ftxui::color(theme.base.dim));

      if (!rolling.bridge_target.empty()) {
        rows.push_back(detailLine("target ", rolling.bridge_target,
                                  theme.base.dim, 48));
      }
      if (!rolling.bridge_hint.empty()) {
        rows.push_back(detailLine("hint   ", rolling.bridge_hint,
                                  theme.base.dim, 48));
      }
    } else if (!model_->memory_labels.empty()) {
      for (const auto &label : model_->memory_labels) {
        rows.push_back(ftxui::text("  " + firmius::shared::ICON_BRAIN + " " + label) |
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
