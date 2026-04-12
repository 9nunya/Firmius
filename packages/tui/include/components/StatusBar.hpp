#ifndef FIRMIUS_COMPONENTS_STATUS_BAR_HPP
#define FIRMIUS_COMPONENTS_STATUS_BAR_HPP

#include "Enums.hpp"
#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>

namespace firmius::tui {

struct StatusBarModel {
  std::string status_text; // e.g. "IDLE", "STREAMING", etc.
  std::string model_name;  // e.g. "nanogpt/gpt-4"
  std::string purpose;     // e.g. "orchestrator"
  std::string title;       // e.g. "Research"
  std::string agent_name;
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
};

ftxui::Component StatusBar(const std::shared_ptr<StatusBarModel> &model);

} // namespace firmius::tui

#endif
