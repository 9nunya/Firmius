#ifndef FIRMIUS_TUI_SKIN_CONFIG_HPP
#define FIRMIUS_TUI_SKIN_CONFIG_HPP

#include <string>
#include <vector>

namespace firmius::tui {

enum class SkinKind {
  Firmius,
  Claudex,
};

inline std::string skinKindToString(SkinKind kind) {
  switch (kind) {
  case SkinKind::Claudex:
    return "claudex";
  case SkinKind::Firmius:
  default:
    return "firmius";
  }
}

inline SkinKind skinKindFromString(const std::string &value) {
  if (value == "claudex" || value == "Claudex") {
    return SkinKind::Claudex;
  }
  return SkinKind::Firmius;
}

inline std::vector<std::string> allSkinNames() {
  return {"firmius", "claudex"};
}

enum class SkinToolDisplayMode {
  Full,
  Compact,
  Minimal,
};

enum class SkinStatusBarMode {
  Powerline,
  Minimal,
  Hidden,
};

enum class SkinDiffDefaultMode {
  Expanded,
  Collapsed,
};

enum class SkinToolResultsMode {
  Expanded,
  Collapsed,
};

enum class SkinQuickToolsDisplayMode {
  Grouped,
  Individual,
  Hidden,
};

enum class SkinGlintSpeed {
  Slow,
  Normal,
  Fast,
};

enum class SkinSpinnerStyle {
  Default,
  Braille,
};

struct SkinConfig {
  SkinKind kind = SkinKind::Firmius;

  bool show_title_bar = true;
  bool show_agent_strip = true;
  bool show_work_panel = true;
  bool show_errors = true;
  bool show_notices = true;

  bool show_turn_numbers = true;
  bool show_turn_footers = true;
  bool show_turn_timing = true;
  bool show_turn_tokens = true;
  bool show_live_footer = true;
  bool show_blank_lines = true;
  bool blank_lines_between_messages = true;
  bool blank_lines_after_user = true;
  bool blank_lines_after_agent = true;
  bool blank_lines_after_tools = true;
  bool show_thinking_blocks = true;
  bool show_thinking = true;
  bool show_thinking_label = true;
  bool show_user_bubble_bg = true;
  bool indent_agent_rows = true;
  bool show_compaction_markers = true;

  SkinToolDisplayMode tool_display = SkinToolDisplayMode::Full;
  SkinToolResultsMode tool_results = SkinToolResultsMode::Expanded;
  int tool_result_preview_lines = 4;
  SkinDiffDefaultMode diffs_default = SkinDiffDefaultMode::Expanded;
  std::string diff_toggle_key;
  SkinQuickToolsDisplayMode quick_tools_display =
      SkinQuickToolsDisplayMode::Grouped;
  int process_output_lines = 4;
  bool show_tool_borders = true;
  bool show_tool_headers = true;
  bool show_tool_body = true;
  bool show_tool_icons = true;
  bool show_tool_status_dots = true;
  bool dim_tool_metadata = false;
  bool glint_tool_icons = false;
  bool glint_tool_blocks = true;
  bool glint_quick_tools = true;

  SkinStatusBarMode status_bar_mode = SkinStatusBarMode::Powerline;
  bool show_token_counts = true;
  bool status_show_processes = true;
  bool compact_input = false;

  bool show_persistent_live_row = false;
  bool live_row_busy_only = false;
  bool live_row_glint = false;
  bool live_row_gradient = false;
  bool live_row_show_elapsed = true;
  bool live_row_show_activity = true;
  bool live_row_show_plan_excerpt = false;
  bool live_row_show_todo_excerpt = false;
  std::string live_row_phrase_bank;
  std::string live_row_mode;
  int live_row_cycle_seconds = 6;

  bool glint_enabled = true;
  SkinGlintSpeed glint_speed = SkinGlintSpeed::Normal;
  bool glint_status_bar = true;
  SkinSpinnerStyle spinner_style = SkinSpinnerStyle::Default;

  bool show_plan_inline = false;
  bool show_todo_inline = false;

  bool claudex_hide_done_footer = false;
  bool claudex_diffs_collapsed_by_default = false;

  bool compactStatusBar() const {
    return status_bar_mode == SkinStatusBarMode::Minimal;
  }

  bool compactToolDisplay() const {
    return tool_display == SkinToolDisplayMode::Compact;
  }

  bool diffsCollapsedByDefault() const {
    return diffs_default == SkinDiffDefaultMode::Collapsed ||
           tool_results == SkinToolResultsMode::Collapsed ||
           claudex_diffs_collapsed_by_default;
  }
};

inline std::string toolDisplayModeToString(SkinToolDisplayMode mode) {
  switch (mode) {
  case SkinToolDisplayMode::Compact:
    return "compact";
  case SkinToolDisplayMode::Minimal:
    return "minimal";
  case SkinToolDisplayMode::Full:
  default:
    return "full";
  }
}

inline SkinToolDisplayMode toolDisplayModeFromString(
    const std::string &value,
    SkinToolDisplayMode fallback = SkinToolDisplayMode::Full) {
  if (value == "compact" || value == "Compact") {
    return SkinToolDisplayMode::Compact;
  }
  if (value == "minimal" || value == "Minimal") {
    return SkinToolDisplayMode::Minimal;
  }
  if (value == "full" || value == "Full" || value == "rich" ||
      value == "Rich") {
    return SkinToolDisplayMode::Full;
  }
  return fallback;
}

inline std::string statusBarModeToString(SkinStatusBarMode mode) {
  switch (mode) {
  case SkinStatusBarMode::Minimal:
    return "minimal";
  case SkinStatusBarMode::Hidden:
    return "hidden";
  case SkinStatusBarMode::Powerline:
  default:
    return "powerline";
  }
}

inline SkinStatusBarMode statusBarModeFromString(
    const std::string &value,
    SkinStatusBarMode fallback = SkinStatusBarMode::Powerline) {
  if (value == "minimal" || value == "Minimal") {
    return SkinStatusBarMode::Minimal;
  }
  if (value == "hidden" || value == "Hidden") {
    return SkinStatusBarMode::Hidden;
  }
  if (value == "powerline" || value == "Powerline") {
    return SkinStatusBarMode::Powerline;
  }
  return fallback;
}

inline std::string diffDefaultModeToString(SkinDiffDefaultMode mode) {
  switch (mode) {
  case SkinDiffDefaultMode::Collapsed:
    return "collapsed";
  case SkinDiffDefaultMode::Expanded:
  default:
    return "expanded";
  }
}

inline SkinDiffDefaultMode diffDefaultModeFromString(
    const std::string &value,
    SkinDiffDefaultMode fallback = SkinDiffDefaultMode::Expanded) {
  if (value == "collapsed" || value == "Collapsed") {
    return SkinDiffDefaultMode::Collapsed;
  }
  if (value == "expanded" || value == "Expanded") {
    return SkinDiffDefaultMode::Expanded;
  }
  return fallback;
}

inline std::string toolResultsModeToString(SkinToolResultsMode mode) {
  switch (mode) {
  case SkinToolResultsMode::Collapsed:
    return "collapsed";
  case SkinToolResultsMode::Expanded:
  default:
    return "expanded";
  }
}

inline SkinToolResultsMode toolResultsModeFromString(
    const std::string &value,
    SkinToolResultsMode fallback = SkinToolResultsMode::Expanded) {
  if (value == "collapsed" || value == "Collapsed") {
    return SkinToolResultsMode::Collapsed;
  }
  if (value == "expanded" || value == "Expanded") {
    return SkinToolResultsMode::Expanded;
  }
  return fallback;
}

inline std::string quickToolsDisplayModeToString(
    SkinQuickToolsDisplayMode mode) {
  switch (mode) {
  case SkinQuickToolsDisplayMode::Individual:
    return "individual";
  case SkinQuickToolsDisplayMode::Hidden:
    return "hidden";
  case SkinQuickToolsDisplayMode::Grouped:
  default:
    return "grouped";
  }
}

inline SkinQuickToolsDisplayMode quickToolsDisplayModeFromString(
    const std::string &value,
    SkinQuickToolsDisplayMode fallback = SkinQuickToolsDisplayMode::Grouped) {
  if (value == "individual" || value == "Individual") {
    return SkinQuickToolsDisplayMode::Individual;
  }
  if (value == "hidden" || value == "Hidden") {
    return SkinQuickToolsDisplayMode::Hidden;
  }
  if (value == "grouped" || value == "Grouped") {
    return SkinQuickToolsDisplayMode::Grouped;
  }
  return fallback;
}

inline std::string glintSpeedToString(SkinGlintSpeed speed) {
  switch (speed) {
  case SkinGlintSpeed::Slow:
    return "slow";
  case SkinGlintSpeed::Fast:
    return "fast";
  case SkinGlintSpeed::Normal:
  default:
    return "normal";
  }
}

inline SkinGlintSpeed glintSpeedFromString(
    const std::string &value,
    SkinGlintSpeed fallback = SkinGlintSpeed::Normal) {
  if (value == "slow" || value == "Slow") {
    return SkinGlintSpeed::Slow;
  }
  if (value == "fast" || value == "Fast") {
    return SkinGlintSpeed::Fast;
  }
  if (value == "normal" || value == "Normal") {
    return SkinGlintSpeed::Normal;
  }
  return fallback;
}

inline float glintIntervalSeconds(SkinGlintSpeed speed) {
  switch (speed) {
  case SkinGlintSpeed::Slow:
    return 5.5f;
  case SkinGlintSpeed::Fast:
    return 1.8f;
  case SkinGlintSpeed::Normal:
  default:
    return 3.5f;
  }
}

inline float glintDurationSeconds(SkinGlintSpeed speed) {
  switch (speed) {
  case SkinGlintSpeed::Slow:
    return 1.8f;
  case SkinGlintSpeed::Fast:
    return 0.9f;
  case SkinGlintSpeed::Normal:
  default:
    return 1.3f;
  }
}

inline float glintTransitionFadeSeconds(SkinGlintSpeed speed) {
  switch (speed) {
  case SkinGlintSpeed::Slow:
    return 0.45f;
  case SkinGlintSpeed::Fast:
    return 0.18f;
  case SkinGlintSpeed::Normal:
  default:
    return 0.28f;
  }
}

inline std::string spinnerStyleToString(SkinSpinnerStyle style) {
  switch (style) {
  case SkinSpinnerStyle::Braille:
    return "braille";
  case SkinSpinnerStyle::Default:
  default:
    return "default";
  }
}

inline SkinSpinnerStyle spinnerStyleFromString(
    const std::string &value,
    SkinSpinnerStyle fallback = SkinSpinnerStyle::Default) {
  if (value == "braille" || value == "Braille") {
    return SkinSpinnerStyle::Braille;
  }
  if (value == "default" || value == "Default") {
    return SkinSpinnerStyle::Default;
  }
  return fallback;
}

inline SkinConfig defaultSkinConfig(SkinKind kind) {
  SkinConfig cfg;
  cfg.kind = kind;
  if (kind == SkinKind::Claudex) {
    cfg.show_title_bar = false;
    cfg.show_agent_strip = false;
    cfg.show_work_panel = false;
    cfg.show_turn_footers = false;
    cfg.show_turn_numbers = false;
    cfg.show_turn_timing = false;
    cfg.show_turn_tokens = false;
    cfg.show_live_footer = false;
    cfg.show_blank_lines = false;
    cfg.blank_lines_between_messages = false;
    cfg.blank_lines_after_user = false;
    cfg.blank_lines_after_agent = false;
    cfg.blank_lines_after_tools = false;
    cfg.show_thinking = true;
    cfg.show_thinking_blocks = true;
    cfg.show_thinking_label = true;
    cfg.show_user_bubble_bg = false;
    cfg.indent_agent_rows = false;
    cfg.show_compaction_markers = true;
    cfg.spinner_style = SkinSpinnerStyle::Braille;
    cfg.tool_display = SkinToolDisplayMode::Compact;
    cfg.tool_results = SkinToolResultsMode::Collapsed;
    cfg.tool_result_preview_lines = 4;
    cfg.diffs_default = SkinDiffDefaultMode::Collapsed;
    cfg.diff_toggle_key = "ctrl+g";
    cfg.quick_tools_display = SkinQuickToolsDisplayMode::Grouped;
    cfg.process_output_lines = 4;
    cfg.show_tool_borders = false;
    cfg.show_tool_headers = false;
    cfg.show_tool_body = true;
    cfg.show_tool_icons = true;
    cfg.show_tool_status_dots = true;
    cfg.dim_tool_metadata = true;
    cfg.glint_tool_icons = true;
    cfg.glint_tool_blocks = false;
    cfg.glint_quick_tools = true;
    cfg.status_bar_mode = SkinStatusBarMode::Minimal;
    cfg.show_token_counts = true;
    cfg.status_show_processes = true;
    cfg.compact_input = true;
    cfg.show_persistent_live_row = true;
    cfg.live_row_busy_only = true;
    cfg.live_row_glint = true;
    cfg.live_row_gradient = true;
    cfg.live_row_show_elapsed = true;
    cfg.live_row_show_activity = true;
    cfg.live_row_show_plan_excerpt = true;
    cfg.live_row_show_todo_excerpt = true;
    cfg.live_row_phrase_bank = "cheeky";
    cfg.live_row_mode = "persistent";
    cfg.live_row_cycle_seconds = 10;
    cfg.glint_enabled = true;
    cfg.glint_speed = SkinGlintSpeed::Slow;
    cfg.spinner_style = SkinSpinnerStyle::Braille;
    cfg.show_plan_inline = true;
    cfg.show_todo_inline = true;
    cfg.claudex_hide_done_footer = true;
    cfg.claudex_diffs_collapsed_by_default = true;
  } else {
    cfg.show_title_bar = true;
    cfg.show_agent_strip = true;
    cfg.show_work_panel = true;
    cfg.show_errors = true;
    cfg.show_notices = true;
    cfg.show_turn_numbers = true;
    cfg.show_turn_footers = true;
    cfg.show_turn_timing = true;
    cfg.show_turn_tokens = true;
    cfg.show_live_footer = true;
    cfg.show_blank_lines = true;
    cfg.blank_lines_between_messages = true;
    cfg.blank_lines_after_user = true;
    cfg.blank_lines_after_agent = true;
    cfg.blank_lines_after_tools = true;
    cfg.show_thinking = true;
    cfg.show_thinking_blocks = true;
    cfg.show_user_bubble_bg = true;
    cfg.indent_agent_rows = true;
    cfg.show_compaction_markers = true;
    cfg.tool_display = SkinToolDisplayMode::Full;
    cfg.tool_results = SkinToolResultsMode::Expanded;
    cfg.tool_result_preview_lines = 4;
    cfg.diffs_default = SkinDiffDefaultMode::Expanded;
    cfg.quick_tools_display = SkinQuickToolsDisplayMode::Grouped;
    cfg.process_output_lines = 4;
    cfg.show_tool_borders = true;
    cfg.show_tool_headers = true;
    cfg.show_tool_body = true;
    cfg.show_tool_icons = true;
    cfg.show_tool_status_dots = true;
    cfg.dim_tool_metadata = false;
    cfg.glint_tool_icons = false;
    cfg.glint_tool_blocks = true;
    cfg.glint_quick_tools = true;
    cfg.status_bar_mode = SkinStatusBarMode::Powerline;
    cfg.show_token_counts = true;
    cfg.status_show_processes = true;
    cfg.compact_input = false;
    cfg.glint_enabled = true;
    cfg.glint_speed = SkinGlintSpeed::Normal;
    cfg.glint_status_bar = true;
    cfg.spinner_style = SkinSpinnerStyle::Default;
    cfg.show_plan_inline = false;
    cfg.show_todo_inline = false;
  }
  return cfg;
}

} // namespace firmius::tui

#endif
