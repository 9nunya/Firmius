#ifndef FIRMIUS_COMPONENTS_STATUS_BAR_HPP
#define FIRMIUS_COMPONENTS_STATUS_BAR_HPP

#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>

namespace firmius::tui {

struct StatusBarModel {
  std::string status_text; // e.g. "IDLE", "STREAMING", etc.
  std::string model_name;  // e.g. "nanogpt/gpt-4"
  std::string purpose;     // e.g. "orchestrator"
  std::string agent_name;
  std::string model_variant;
  uint32_t context_used = 0;
  uint32_t context_max = 0;
  bool is_active = false;
};

ftxui::Component StatusBar(const std::shared_ptr<StatusBarModel> &model);

} // namespace firmius::tui

#endif
