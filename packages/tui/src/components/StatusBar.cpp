#include "components/StatusBar.hpp"
#include "ThemeManager.hpp"
#include "components/GlintEffect.hpp"
#include "utils/Icons.hpp"
#include "utils/ModelUtil.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
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

std::string
permissionModeToCompactLabel(firmius::shared::ThreadPermissionMode mode) {
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
    ch = next_title
             ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))
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

std::string statusIconForMode(const std::string &mode) {
  if (mode == "streaming")
    return "●";
  if (mode == "executing_tool")
    return "⚙";
  if (mode == "provider_waiting")
    return "…";
  if (mode == "error")
    return "⚠";
  if (mode == "cancelled")
    return "✖";
  return "";
}

size_t decodeUtf8Codepoint(const std::string &text, size_t index,
                           size_t *next_index) {
  const unsigned char ch = static_cast<unsigned char>(text[index]);
  if (ch < 0x80) {
    *next_index = index + 1;
    return ch;
  }
  if ((ch & 0xE0) == 0xC0 && index + 1 < text.size()) {
    *next_index = index + 2;
    return ((ch & 0x1F) << 6) |
           (static_cast<unsigned char>(text[index + 1]) & 0x3F);
  }
  if ((ch & 0xF0) == 0xE0 && index + 2 < text.size()) {
    *next_index = index + 3;
    return ((ch & 0x0F) << 12) |
           ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[index + 2]) & 0x3F);
  }
  if ((ch & 0xF8) == 0xF0 && index + 3 < text.size()) {
    *next_index = index + 4;
    return ((ch & 0x07) << 18) |
           ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[index + 3]) & 0x3F);
  }
  *next_index = index + 1;
  return ch;
}

int estimatedCellWidth(size_t codepoint) {
  if (codepoint == 0) {
    return 0;
  }
  if (codepoint < 0x1100) {
    return 1;
  }
  // Good enough for terminal budgeting: emoji and CJK generally occupy two
  // cells, while ASCII/model/token labels dominate the status row.
  return 2;
}

size_t displayBudget(const std::string &text) {
  size_t width = 0;
  for (size_t i = 0; i < text.size();) {
    size_t next = i + 1;
    width += estimatedCellWidth(decodeUtf8Codepoint(text, i, &next));
    i = next;
  }
  return width;
}

std::string truncateDisplay(const std::string &text, size_t max_cells) {
  if (max_cells == 0) {
    return "";
  }
  if (displayBudget(text) <= max_cells) {
    return text;
  }
  if (max_cells <= 1) {
    return "";
  }

  std::string out;
  size_t used = 0;
  for (size_t i = 0; i < text.size();) {
    size_t next = i + 1;
    const size_t codepoint = decodeUtf8Codepoint(text, i, &next);
    const int width = estimatedCellWidth(codepoint);
    if (used + static_cast<size_t>(width) + 1 > max_cells) {
      break;
    }
    out.append(text, i, next - i);
    used += static_cast<size_t>(width);
    i = next;
  }
  out += "…";
  return out;
}

std::string removeCharacters(std::string text, const std::string &chars) {
  text.erase(std::remove_if(text.begin(), text.end(),
                            [&](char ch) {
                              return chars.find(ch) != std::string::npos;
                            }),
             text.end());
  return text;
}

std::string replaceAll(std::string text, const std::string &from,
                       const std::string &to) {
  if (from.empty()) {
    return text;
  }
  size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

std::string compactAgentLabel(const firmius::tui::StatusBarModel &model,
                              int tier) {
  std::string agent = model.agent_name.empty()
                          ? "Aster"
                          : firmius::shared::PrettifyModelName(model.agent_name);
  if (tier >= 3) {
    return truncateDisplay(agent, 1);
  }
  if (tier >= 2) {
    return truncateDisplay(agent, 10);
  }
  return agent;
}

std::string compactFreeLabel(const std::string &text, int tier,
                             size_t normal_cells, size_t tight_cells) {
  if (text.empty()) {
    return "";
  }
  if (tier >= 3) {
    return truncateDisplay(text, tight_cells);
  }
  if (tier >= 2) {
    return truncateDisplay(text, normal_cells);
  }
  return text;
}

std::string compactModeLabel(const firmius::tui::StatusBarModel &model,
                             int tier) {
  if (model.active_mode.empty()) {
    return "";
  }

  std::string mode = model.active_mode;
  if (tier >= 2) {
    const size_t colon = mode.rfind(':');
    if (colon != std::string::npos && colon + 1 < mode.size()) {
      mode = mode.substr(colon + 1);
    }
  }
  mode = truncateDisplay(mode, tier >= 3 ? 8 : (tier >= 2 ? 12 : 18));

  if (model.active_mode_glyph.empty()) {
    return mode;
  }
  return tier >= 3 ? model.active_mode_glyph + mode
                   : model.active_mode_glyph + " " + mode;
}

std::string compactModelLabel(const firmius::tui::StatusBarModel &model,
                              int tier) {
  std::string text = buildModelText(model, /*compact_mode=*/true);
  if (tier == 0) {
    return text;
  }

  text = replaceAll(text, " (", "(");
  text = replaceAll(text, ")", "");
  if (tier >= 2) {
    text = replaceAll(text, "(Thinking", "T");
    text = replaceAll(text, "(Reasoning", "R");
    text = replaceAll(text, "(Medium", "M");
    text = replaceAll(text, "(High", "H");
    text = replaceAll(text, "(Low", "L");
    text = replaceAll(text, " Thinking", "T");
    text = replaceAll(text, " Reasoning", "R");
    text = replaceAll(text, " Medium", "M");
    text = replaceAll(text, " High", "H");
    text = replaceAll(text, " Low", "L");
    text = replaceAll(text, " Mini", "M");
    text = replaceAll(text, " Codex", "Cx");
    text = removeCharacters(text, " ()");
  }
  return truncateDisplay(text, tier >= 3 ? 12 : 18);
}

std::string compactTokenLabel(const firmius::tui::StatusBarModel &model,
                              int tier) {
  const std::string prompt_summary = formatPromptSummary(model);
  if (prompt_summary.empty() && model.completion_tokens == 0) {
    return "";
  }

  std::string out;
  if (!prompt_summary.empty()) {
    out += tier >= 2 ? "P" : "↑";
    out += prompt_summary;
  }
  if (model.completion_tokens > 0) {
    if (!out.empty()) {
      out += tier >= 3 ? "" : " ";
    }
    out += tier >= 2 ? "C" : "↓";
    out += formatCompactCount(model.completion_tokens);
  }
  return out;
}

std::string compactContextLabel(const firmius::tui::StatusBarModel &model,
                                int tier) {
  if (model.context_max == 0) {
    return "";
  }

  if (model.context_max > 0) {
    const float ratio = static_cast<float>(model.context_used) /
                        static_cast<float>(model.context_max);
    const int pct =
        static_cast<int>(std::lround(std::clamp(ratio, 0.0f, 1.0f) * 100.0f));
    const std::string used = formatCompactCount(model.context_used);
    std::string max = formatCompactCount(model.context_max);
    if (tier >= 3) {
      max = replaceAll(max, ".0M", "M");
    }
    std::string text = used + "/" + max;
    if (tier >= 3) {
      return "X" + text + std::to_string(pct) + "%";
    }
    if (tier >= 2) {
      return "X" + text + " " + std::to_string(pct) + "%";
    }
    return text + " (" + std::to_string(pct) + "%)";
  }
  return "";
}

std::string compactQuotaLabel(const firmius::tui::StatusBarModel &model,
                              int tier) {
  std::string quota =
      !model.quota_usage.empty() ? model.quota_usage : model.bucket_summary;
  if (quota.empty()) {
    return "";
  }
  if (tier >= 3) {
    quota = removeCharacters(quota, " ");
  }
  return truncateDisplay(quota, tier >= 3 ? 12 : (tier >= 2 ? 18 : 28));
}

std::string compactProcessLabel(const firmius::tui::StatusBarModel &model,
                                int tier) {
  if (model.live_processes == 0 && model.background_processes == 0) {
    return "";
  }
  if (tier >= 2) {
    return "p" + std::to_string(model.live_processes) + "/" +
           std::to_string(model.background_processes);
  }
  std::string out = "proc";
  if (model.live_processes > 0) {
    out += " " + std::to_string(model.live_processes) + " live";
  }
  if (model.background_processes > 0) {
    out += " " + std::to_string(model.background_processes) + " bg";
  }
  return out;
}

std::string joinStatusParts(const std::vector<std::string> &parts,
                            const std::string &separator) {
  std::string out;
  for (const auto &part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!out.empty()) {
      out += separator;
    }
    out += part;
  }
  return out;
}

std::string buildAdaptiveStatusText(const firmius::tui::StatusBarModel &model,
                                    int width, bool claudex_skin) {
  if (!model.custom_status_text.empty()) {
    return truncateDisplay(model.custom_status_text,
                           static_cast<size_t>(std::max(1, width)));
  }

  const size_t target = static_cast<size_t>(std::max(24, width - 1));
  for (int tier = width < 72 ? 2 : 0; tier <= 3; ++tier) {
    std::vector<std::string> parts;
    const std::string icon = statusIconForMode(model.status_text);
    if (claudex_skin) {
      const std::string agent = compactAgentLabel(model, tier);
      parts.push_back(icon.empty() ? agent : icon + " " + agent);
    } else {
      const std::string purpose =
          compactFreeLabel(model.purpose.empty() ? model.agent_name
                                                 : model.purpose,
                           tier, 10, 1);
      parts.push_back(icon.empty() ? purpose : icon + " " + purpose);
      parts.push_back(compactFreeLabel(model.title, tier, 10, 1));
    }
    parts.push_back(compactModeLabel(model, tier));
    parts.push_back(compactModelLabel(model, tier));
    parts.push_back(compactTokenLabel(model, tier));
    parts.push_back(compactContextLabel(model, tier));
    parts.push_back(compactQuotaLabel(model, tier));
    parts.push_back(compactProcessLabel(model, tier));
    parts.push_back(permissionModeToCompactLabel(model.permission_mode));

    const std::string sep = tier >= 2 ? "·" : " · ";
    std::string line = (claudex_skin ? " " : "") + joinStatusParts(parts, sep);
    if (displayBudget(line) <= target || tier == 3) {
      return truncateDisplay(line, target);
    }
  }

  return "";
}

std::vector<std::string> trimmedHookLines(const firmius::tui::StatusBarModel &model,
                                          int width) {
  std::vector<std::string> out;
  const std::size_t max_cells = static_cast<std::size_t>(std::max(8, width - 1));
  const int cap = std::max(0, model.max_status_lines - 1);
  for (int i = static_cast<int>(model.hook_status_lines.size()) - 1;
       i >= 0 && static_cast<int>(out.size()) < cap; --i) {
    out.push_back(truncateDisplay(model.hook_status_lines[static_cast<std::size_t>(i)],
                                  max_cells));
  }
  std::reverse(out.begin(), out.end());
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
    int term_width = std::max(40, ftxui::Terminal::Size().dimx);
    bool compact_mode = term_width <= 110;
    bool ultra_compact = term_width <= 72;

    // Minimal status bar mode (Claudex default).
    if (model_->compact_skin_mode) {
      const std::string minimal_text =
          buildAdaptiveStatusText(*model_, term_width, /*claudex_skin=*/true);
      ftxui::Elements rows;
      rows.push_back(ftxui::text(minimal_text) | ftxui::bold |
                     ftxui::color(theme.base.fg) | ftxui::bgcolor(theme.base.bg));
      for (const auto &line : trimmedHookLines(*model_, term_width)) {
        rows.push_back(ftxui::text(line) | ftxui::color(theme.base.dim) |
                       ftxui::bgcolor(theme.base.bg));
      }
      return ftxui::vbox(std::move(rows)) |
             ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, term_width);
    }

    syncGlint(compact_mode);

    if (term_width <= 92) {
      const std::string compact_text =
          buildAdaptiveStatusText(*model_, term_width, /*claudex_skin=*/false);
      ftxui::Elements rows;
      rows.push_back(ftxui::text(compact_text) | ftxui::bold |
                     ftxui::color(theme.base.fg) |
                     ftxui::bgcolor(theme.status_bar.filler_bg));
      for (const auto &line : trimmedHookLines(*model_, term_width)) {
        rows.push_back(ftxui::text(line) | ftxui::color(theme.base.dim) |
                       ftxui::bgcolor(theme.status_bar.filler_bg));
      }
      return ftxui::vbox(std::move(rows)) |
             ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, term_width);
    }

    // Powerline status bar (Firmius default)
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

    // 4. Mode Segment (optional). Rendered between title and model so
    // the operator can see at a glance which stance the focused agent
    // (or the welcome-screen pre-pick) is in. Hidden entirely when
    // active_mode is empty so the bar doesn't gain noise.
    ftxui::Element mode_el = ftxui::text("");
    ftxui::Element sep_mode = ftxui::text("");
    ftxui::Color mode_bg = theme.agent_strip.pills.purpose_bg;
    ftxui::Color mode_fg = theme.agent_strip.pills.purpose_fg;
    if (!model_->active_mode.empty()) {
      std::string mode_label = model_->active_mode;
      if (compact_mode && mode_label.length() > 14) {
        mode_label = mode_label.substr(0, 14);
      }
      // Prepend the glyph so the powerline pill matches the visual
      // shorthand declared in the mode's frontmatter.
      const std::string glyph_prefix =
          model_->active_mode_glyph.empty()
              ? std::string{}
              : model_->active_mode_glyph + " ";
      mode_el = ftxui::text(" " + glyph_prefix + mode_label + " ") |
                ftxui::bold | ftxui::color(mode_fg) | ftxui::bgcolor(mode_bg);
      const ftxui::Color sep_left_bg =
          model_->title.empty() ? purpose_bg : title_bg;
      sep_mode = ftxui::text(firmius::shared::PL_LEFT_SEP) |
                 ftxui::color(sep_left_bg) | ftxui::bgcolor(mode_bg);
    }

    // 5. Model Segment
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

    // Pick the separator that flows into the model pill. If the mode
    // pill is showing it owns the right edge before the model; otherwise
    // either the title or purpose feeds straight into model.
    auto sep_before_model = !model_->active_mode.empty()
                                ? (ftxui::text(firmius::shared::PL_LEFT_SEP) |
                                   ftxui::color(mode_bg) |
                                   ftxui::bgcolor(pill_bg))
                            : model_->title.empty() ? sep_purpose_model
                                                    : sep_title_model;
    auto sep_after_purpose =
        model_->title.empty() ? sep_purpose_model : sep_title_model;

    // 5. Context Segment (Right Aligned)
    ftxui::Color ctx_bg = theme.status_bar.context.bg;
    std::string perms_label =
        permissionModeToCompactLabel(model_->permission_mode);
    ftxui::Color perms_color =
        permissionModeColor(model_->permission_mode, theme);
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
        std::string combined_ctx = formatCompactCount(model_->context_used) +
                                   " / " +
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
        token_text +=
            std::string("↓ ") + formatCompactCount(model_->completion_tokens);
      }

      auto token_bg = theme.agent_strip.pills.model_bg;
      auto token_fg = theme.agent_strip.pills.model_fg;
      token_seg = ftxui::hbox({
          ftxui::text(firmius::shared::PL_RIGHT_SEP) | ftxui::color(token_bg) |
              ftxui::bgcolor(current_bg),
          ftxui::text(token_text + " ") | ftxui::bold | ftxui::color(token_fg) |
              ftxui::bgcolor(token_bg),
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
          ftxui::text(firmius::shared::PL_RIGHT_SEP) | ftxui::color(quota_bg) |
              ftxui::bgcolor(current_bg),
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

    // The mode pill (when present) sits between title and model and is
    // joined by sep_mode on its left and sep_before_model on its right.
    // When the mode pill is hidden, sep_after_purpose still wires title
    // straight into model, preserving the original layout.
    ftxui::Elements rows;
    rows.push_back(
        ftxui::hbox({status_seg, sep_status_purpose, purpose_el, sep_title,
                     title_el, sep_mode, mode_el,
                     model_->active_mode.empty() ? sep_after_purpose
                                                 : sep_before_model,
                     model_el, sep_model_filler,
                     ftxui::filler() | ftxui::bgcolor(filler_bg), token_seg,
                     quota_seg, process_seg, ctx_seg}));
    for (const auto &line : trimmedHookLines(*model_, term_width)) {
      rows.push_back(ftxui::text(line) | ftxui::color(theme.base.dim) |
                     ftxui::bgcolor(filler_bg));
    }
    return ftxui::vbox(std::move(rows));
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
            ? std::vector<ftxui::Color>{theme.status_bar.pill_fg,
                                        theme.base.highlight}
            : state.glint;
    cfg.glintSize = 14;
    cfg.intervalSeconds = 5;
    cfg.durationSeconds = 0.8f;
    cfg.easing = GlintEasing::EaseOut;

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
