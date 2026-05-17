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

    if (!model_->memory_labels.empty()) {
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
