#include "components/StatusBar.hpp"
#include "ThemeManager.hpp"
#include "components/GlintEffect.hpp"
#include "utils/Icons.hpp"
#include "utils/ModelUtil.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <vector>

namespace firmius::tui {

namespace {

const StateColors &GetStateColorsForMode(const std::string &mode,
                                         const Theme &theme) {
  if (mode == "streaming")
    return theme.status_bar.streaming;
  if (mode == "executing_tool")
    return theme.status_bar.executing_tool;
  if (mode == "provider_waiting")
    return theme.status_bar.provider_waiting;
  if (mode == "compacting")
    return theme.status_bar.compacting;
  if (mode == "error" || mode == "cancelled")
    return theme.status_bar.error;
  return theme.status_bar.idle;
}

std::string permissionModeToCompactLabel(
    firmius::shared::ThreadPermissionMode mode) {
  switch (mode) {
  case firmius::shared::ThreadPermissionMode::Request:
    return "ASK";
  case firmius::shared::ThreadPermissionMode::AlwaysAllow:
    return "AUTO";
  case firmius::shared::ThreadPermissionMode::DenyAll:
    return "DENY";
  }
  return "ASK";
}

ftxui::Color permissionModeColor(firmius::shared::ThreadPermissionMode mode,
                                 const Theme &theme) {
  switch (mode) {
  case firmius::shared::ThreadPermissionMode::Request:
    return theme.status_bar.context.icon;
  case firmius::shared::ThreadPermissionMode::AlwaysAllow:
    return theme.modals.highlight_fg;
  case firmius::shared::ThreadPermissionMode::DenyAll:
    return theme.status_bar.error.normal.fg;
  }
  return theme.status_bar.context.icon;
}

std::string prettifyVariantName(const std::string &variant) {
  if (variant.empty()) {
    return "";
  }
  std::string out = variant;
  for (char &ch : out) {
    if (ch == '_' || ch == '-') {
      ch = ' ';
    }
  }
  bool next_title = true;
  for (char &ch : out) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      next_title = true;
      continue;
    }
    ch = next_title ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))
                    : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    next_title = false;
  }
  return out;
}

std::string buildModelText(const firmius::tui::StatusBarModel &model,
                           bool compact_mode) {
  std::string model_text = firmius::shared::PrettifyModelName(model.model_name);

  if (compact_mode) {
    size_t last_space = model_text.rfind(' ');
    if (last_space != std::string::npos) {
      std::string last_word = model_text.substr(last_space + 1);
      if (last_word == "Mini" || last_word == "Pro" || last_word == "Max" ||
          last_word == "Ultra" || last_word == "Nano") {
        model_text = model_text.substr(0, last_space);
      }
    }
  }

  const std::string variant = prettifyVariantName(model.model_variant);
  if (!variant.empty()) {
    model_text += " (" + variant + ")";
  }

  return model_text;
}

std::string formatCompactCount(uint32_t value) {
  if (value >= 1000000) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fM", value / 1000000.0f);
    return buf;
  }
  if (value >= 1000) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fk", value / 1000.0f);
    return buf;
  }
  return std::to_string(value);
}

std::string formatPromptSummary(const firmius::tui::StatusBarModel &model) {
  if (model.sent_prompt == 0 && model.billed_prompt == 0) {
    return "";
  }

  const uint32_t visiblePrompt =
      model.sent_prompt > 0 ? model.sent_prompt : model.billed_prompt;
  std::string summary = formatCompactCount(visiblePrompt);
  if (model.sent_prompt > 0 && model.billed_prompt > 0 &&
      model.sent_prompt != model.billed_prompt) {
    summary += "/" + formatCompactCount(model.billed_prompt);
  }
  return summary;
}


std::string buildCompactMinimalStatusText(const firmius::tui::StatusBarModel &model,
                                         bool ultra_compact) {
  // Minimal/Claudex status bar format:
  //   aster · opus-4 · ↑45.2k ↓3.1k · 34% · ASK
  // Includes quota usage when present.

  std::string out;
  if (!model.agent_name.empty()) {
    out += model.agent_name;
  }

  const std::string model_text = buildModelText(model, /*compact_mode=*/true);
  if (!model_text.empty()) {
    if (!out.empty()) {
      out += " · ";
    }
    out += model_text;
  }

  const std::string prompt_summary = formatPromptSummary(model);
  if (!prompt_summary.empty() || model.completion_tokens > 0) {
    if (!out.empty()) {
      out += " · ";
    }
    if (!prompt_summary.empty()) {
      out += std::string("↑") + prompt_summary;
    }
    if (model.completion_tokens > 0) {
      if (!prompt_summary.empty()) {
        out += " ";
      }
      out += std::string("↓") + formatCompactCount(model.completion_tokens);
    }
  }

  if (!model.quota_usage.empty()) {
    if (!out.empty()) {
      out += " · ";
    }
    out += model.quota_usage;
  }

  if (model.context_max > 0) {
    const float ratio =
        static_cast<float>(model.context_used) / static_cast<float>(model.context_max);
    const float display_ratio = std::clamp(ratio, 0.0f, 1.0f);
    const int pct = static_cast<int>(std::lround(display_ratio * 100.0f));
    if (!out.empty()) {
      out += " · ";
    }
    out += std::to_string(pct) + "%";
  }

  if (!out.empty()) {
    out += " · ";
  }
  out += permissionModeToCompactLabel(model.permission_mode);

  if (ultra_compact && out.size() > 70) {
    out.resize(67);
    out += "...";
  }
  return out;
}
class StatusBarComponentBase : public ftxui::ComponentBase {
public:
  explicit StatusBarComponentBase(std::shared_ptr<StatusBarModel> model)
      : model_(std::move(model)) {}

  ftxui::Element OnRender() override {
    if (!model_) {
      return ftxui::text("");
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    std::string mode = model_->status_text;
    const auto &state = GetStateColorsForMode(mode, theme);

    // Get terminal dimensions for responsive layout
    int term_width = ftxui::Terminal::Size().dimx;
    bool compact_mode = term_width <= 110;
    bool ultra_compact = term_width < 70;


    // Minimal status bar mode (Claudex default).
    if (model_->compact_skin_mode) {
      syncGlint(/*compact_mode=*/true);
      const std::string minimal_text =
          buildCompactMinimalStatusText(*model_, ultra_compact);
      return ftxui::text(minimal_text) | ftxui::bold |
             ftxui::color(theme.base.fg) | ftxui::bgcolor(theme.base.bg) |
             ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1);
    }
    syncGlint(compact_mode);

    // 1. Status Segment (Icon)
    std::string status_icon = firmius::shared::ICON_WAIT;
    if (mode == "streaming" || mode == "executing_tool")
      status_icon = firmius::shared::ICON_GEAR;
    else if (mode == "error" || mode == "cancelled")
      status_icon = firmius::shared::ICON_ERROR;

    ftxui::Color status_bg = state.normal.bg;
    ftxui::Color status_fg = state.normal.fg;

    auto status_seg = ftxui::text(" " + status_icon + " ") | ftxui::bold |
                      ftxui::color(status_fg) | ftxui::bgcolor(status_bg);

    // Colors for subsequent segments
    ftxui::Color purpose_bg = theme.agent_strip.pills.purpose_bg;
    ftxui::Color purpose_fg = theme.agent_strip.pills.purpose_fg;
    ftxui::Color title_bg = theme.agent_strip.pills.slug_bg;
    ftxui::Color title_fg = theme.agent_strip.pills.slug_fg;
    ftxui::Color pill_bg = theme.status_bar.pill_bg;
    ftxui::Color pill_fg = theme.status_bar.pill_fg;
    ftxui::Color filler_bg = theme.status_bar.filler_bg;

    // Separators
    auto sep_status_purpose = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                              ftxui::color(status_bg) |
                              ftxui::bgcolor(purpose_bg);
    auto sep_purpose_title = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                             ftxui::color(purpose_bg) |
                             ftxui::bgcolor(title_bg);
    auto sep_title_model = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                           ftxui::color(title_bg) | ftxui::bgcolor(pill_bg);
    auto sep_purpose_model = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                             ftxui::color(purpose_bg) | ftxui::bgcolor(pill_bg);
    auto sep_model_filler = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                            ftxui::color(pill_bg) | ftxui::bgcolor(filler_bg);

    // 2. Purpose Segment
    std::string purpose = model_->purpose;
    if (compact_mode && purpose.length() > 8) {
      purpose = purpose.substr(0, 8);
    }
    auto purpose_el = ftxui::text(" " + purpose + " ") | ftxui::bold |
                      ftxui::color(purpose_fg) | ftxui::bgcolor(purpose_bg);

    // 3. Title Segment (optional)
    ftxui::Element title_el = ftxui::text("");
    ftxui::Element sep_title = ftxui::text("");
    if (!model_->title.empty()) {
      std::string title = model_->title;
      if (compact_mode && title.length() > 10) {
        title = title.substr(0, 10);
      }
      title_el = ftxui::text(" " + title + " ") | ftxui::bold |
                 ftxui::color(title_fg) | ftxui::bgcolor(title_bg);
      sep_title = sep_purpose_title;
    }

    // 4. Model Segment
    std::string model_text = buildModelText(*model_, compact_mode);
    ftxui::Element model_el;
    bool is_working = (mode == "streaming" || mode == "executing_tool" ||
                       mode == "provider_waiting" || mode == "compacting");
    if (is_working && glint_) {
      model_el = glint_->Render() | ftxui::bgcolor(pill_bg);
    } else {
      model_el = ftxui::text(" " + model_text + " ") | ftxui::color(pill_fg) |
                 ftxui::bgcolor(pill_bg);
    }

    auto sep_after_purpose =
        model_->title.empty() ? sep_purpose_model : sep_title_model;

    // 5. Context Segment (Right Aligned)
    ftxui::Color ctx_bg = theme.status_bar.context.bg;
    std::string perms_label = permissionModeToCompactLabel(model_->permission_mode);
    ftxui::Color perms_color = permissionModeColor(model_->permission_mode, theme);
    ftxui::Elements ctx_items;

    ctx_items.push_back(ftxui::text(" " + perms_label + " ") | ftxui::bold |
                        ftxui::color(perms_color) | ftxui::bgcolor(ctx_bg));

    if (model_->context_max > 0) {
      float ratio =
          static_cast<float>(model_->context_used) / model_->context_max;
      float display_ratio = std::clamp(ratio, 0.0f, 1.0f);
      ftxui::Color ctx_color = theme.status_bar.context.low;
      if (display_ratio > 0.85f)
        ctx_color = theme.status_bar.context.high;
      else if (display_ratio > 0.60f)
        ctx_color = theme.status_bar.context.medium;

      char buf[32];
      snprintf(buf, sizeof(buf), "%.0f%%", display_ratio * 100.0f);
      std::string pct_str = buf;

      if (compact_mode || ultra_compact) {
        ctx_items.push_back(ftxui::text(firmius::shared::PL_RIGHT_SOFT_SEP) |
                            ftxui::color(theme.base.dim) |
                            ftxui::bgcolor(ctx_bg));
        ctx_items.push_back(ftxui::text(" " + pct_str + " ") | ftxui::bold |
                            ftxui::color(ctx_color) | ftxui::bgcolor(ctx_bg));
      } else {
        std::string combined_ctx =
            formatCompactCount(model_->context_used) + " / " +
            formatCompactCount(model_->context_max);

        ctx_items.push_back(ftxui::text(firmius::shared::PL_RIGHT_SOFT_SEP) |
                            ftxui::color(theme.base.dim) |
                            ftxui::bgcolor(ctx_bg));
        ctx_items.push_back(ftxui::text(" " + firmius::shared::ICON_CONTEXT +
                                        " " + combined_ctx + " ") |
                            ftxui::color(theme.status_bar.context.icon) |
                            ftxui::bgcolor(ctx_bg));
        ctx_items.push_back(ftxui::text(firmius::shared::PL_RIGHT_SOFT_SEP) |
                            ftxui::color(theme.base.dim) |
                            ftxui::bgcolor(ctx_bg));
        ctx_items.push_back(ftxui::text(" " + pct_str + " ") | ftxui::bold |
                            ftxui::color(ctx_color) | ftxui::bgcolor(ctx_bg));
      }
    }

    ftxui::Color current_bg = filler_bg;

    ftxui::Element token_seg = ftxui::text("");
    const std::string prompt_summary = formatPromptSummary(*model_);
    if (!prompt_summary.empty() || model_->completion_tokens > 0) {
      std::string token_text;
      if (!prompt_summary.empty()) {
        token_text += std::string(" ↑ ") + prompt_summary;
      }
      if (model_->completion_tokens > 0) {
        if (!token_text.empty()) {
          token_text += " ";
        }
        token_text += std::string("↓ ") +
                      formatCompactCount(model_->completion_tokens);
      }

      auto token_bg = theme.agent_strip.pills.model_bg;
      auto token_fg = theme.agent_strip.pills.model_fg;
      token_seg = ftxui::hbox({
          ftxui::text(firmius::shared::PL_RIGHT_SEP) |
              ftxui::color(token_bg) | ftxui::bgcolor(current_bg),
          ftxui::text(token_text + " ") | ftxui::bold |
              ftxui::color(token_fg) | ftxui::bgcolor(token_bg),
      });
      current_bg = token_bg;
    }

    ftxui::Element process_seg = ftxui::text("");
    if (model_->live_processes > 0 || model_->background_processes > 0) {
      std::string process_text = " " + firmius::shared::ICON_TERMINAL;
      if (model_->live_processes > 0) {
        process_text += " " + std::to_string(model_->live_processes);
        if (!ultra_compact) {
          process_text += " live";
        }
      }
      if (model_->background_processes > 0) {
        process_text += " " + std::to_string(model_->background_processes);
        process_text += ultra_compact ? "b" : " bg";
      }

      auto process_bg = theme.status_bar.executing_tool.normal.bg;
      auto process_fg = theme.status_bar.executing_tool.normal.fg;
      process_seg = ftxui::hbox({
          ftxui::text(firmius::shared::PL_RIGHT_SEP) |
              ftxui::color(process_bg) | ftxui::bgcolor(current_bg),
          ftxui::text(process_text + " ") | ftxui::bold |
              ftxui::color(process_fg) | ftxui::bgcolor(process_bg),
      });
      current_bg = process_bg;
    }

    ftxui::Element quota_seg = ftxui::text("");
    if (!model_->quota_usage.empty()) {
      auto quota_bg = theme.agent_strip.pills.model_bg;
      auto quota_fg = theme.agent_strip.pills.model_fg;
      quota_seg = ftxui::hbox({
          ftxui::text(firmius::shared::PL_RIGHT_SEP) |
              ftxui::color(quota_bg) | ftxui::bgcolor(current_bg),
          ftxui::text(" " + model_->quota_usage + " ") | ftxui::bold |
              ftxui::color(quota_fg) | ftxui::bgcolor(quota_bg),
      });
      current_bg = quota_bg;
    }

    ftxui::Element ctx_seg = ftxui::hbox({
        ftxui::text(firmius::shared::PL_RIGHT_SEP) | ftxui::color(ctx_bg) |
            ftxui::bgcolor(current_bg),
        ftxui::hbox(std::move(ctx_items)) | ftxui::bgcolor(ctx_bg),
    });

    return ftxui::hbox({status_seg,
                        sep_status_purpose,
                        purpose_el,
                        sep_title,
                        title_el,
                        sep_after_purpose,
                        model_el,
                        sep_model_filler,
                        ftxui::filler() | ftxui::bgcolor(filler_bg),
                        token_seg,
                        quota_seg,
                        process_seg,
                        ctx_seg}) |
           ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1);
  }

private:
  void syncGlint(bool compact_mode) {
    using namespace firmius::shared;
    std::string model_text = buildModelText(*model_, compact_mode);

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    if (model_text == cached_name_ && model_->status_text == cached_status_ &&
        theme.name == cached_theme_name_ && glint_)
      return;

    cached_name_ = model_text;
    cached_status_ = model_->status_text;
    cached_theme_name_ = theme.name;

    const auto &state = GetStateColorsForMode(model_->status_text, theme);

    GlintConfig cfg;
    cfg.target = GlintConfig::Target::Text;
    cfg.gradientColors =
        state.glint.empty()
            ? std::vector<ftxui::Color>{theme.status_bar.pill_fg, theme.base.highlight}
            : state.glint;
    cfg.glintSize = 14;
    cfg.intervalSeconds = 5;
    cfg.durationSeconds = 2.8f;
    cfg.easing = GlintEasing::EaseInOut;

    glint_ = GlintEffect(ftxui::text(" " + model_text + " ") |
                             ftxui::color(theme.status_bar.pill_fg),
                         cfg);
  }

  std::shared_ptr<StatusBarModel> model_;
  ftxui::Component glint_;
  std::string cached_name_;
  std::string cached_status_;
  std::string cached_theme_name_;
};

} // namespace

ftxui::Component StatusBar(const std::shared_ptr<StatusBarModel> &model) {
  return std::make_shared<StatusBarComponentBase>(model);
}

} // namespace firmius::tui
