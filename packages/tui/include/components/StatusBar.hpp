#ifndef FIRMIUS_COMPONENTS_STATUS_BAR_HPP
#define FIRMIUS_COMPONENTS_STATUS_BAR_HPP

#include "Enums.hpp"
#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

struct StatusBarModel {
  std::string status_text; // e.g. "IDLE", "STREAMING", etc.
  std::string model_name;  // e.g. "nanogpt/gpt-4"
  std::string purpose;     // e.g. "orchestrator"
  std::string title;       // e.g. "Research"
  std::string agent_name;
  /// Currently active mode (qualified, e.g. "forge:apply" or bare like
  /// "diagnose"). On Welcome this mirrors the user's pre-thread pick;
  /// mid-thread it tracks the focused agent's `state.activeMode`. Empty
  /// means "no mode" — the pill is hidden in that case.
  std::string active_mode;
  /// Optional glyph for the active mode (one char or emoji). Looked up
  /// from `Mode::glyph` in the registry. Rendered before the mode name
  /// when present so the operator can scan stance at a glance.
  std::string active_mode_glyph;
  std::string model_variant;
  firmius::shared::ThreadPermissionMode permission_mode =
      firmius::shared::ThreadPermissionMode::Request;
  uint32_t context_used = 0;
  uint32_t context_max = 0;
  uint32_t sent_prompt = 0;
  uint32_t billed_prompt = 0;
  uint32_t completion_tokens = 0;
  double estimated_cost_usd = 0.0;
  std::string account_label;
  std::string quota_usage;
  std::string bucket_summary;
  bool is_active = false;
  int live_processes = 0;
  int background_processes = 0;
  bool compact_skin_mode = false;
  std::string custom_status_text;
  std::vector<std::string> hook_status_lines;
  int max_status_lines = 3;
};

ftxui::Component StatusBar(const std::shared_ptr<StatusBarModel> &model);

} // namespace firmius::tui

#endif
