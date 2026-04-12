#ifndef FIRMIUS_COMPONENTS_CONTEXT_LANE_HPP
#define FIRMIUS_COMPONENTS_CONTEXT_LANE_HPP

#include <cstdint>
#include <ftxui/component/component_base.hpp>
#include <memory>
#include <vector>

namespace firmius::tui {

struct RollingMemoryLaneModel {
  bool enabled = false;
  std::string mode_label;
  std::string preset_label;
  std::string model_label;
  std::uint32_t context_window_tokens = 0;
  float context_occupancy_ratio = 0.0f;
  float buffer_threshold_ratio = 0.0f;
  float target_threshold_ratio = 0.0f;
  float emergency_threshold_ratio = 0.0f;
  std::uint32_t buffer_threshold_tokens = 0;
  std::uint32_t target_threshold_tokens = 0;
  std::uint32_t emergency_threshold_tokens = 0;
  std::uint32_t retained_tail_tokens = 0;
  std::size_t active_observations = 0;
  std::size_t buffered_observations = 0;
  std::size_t active_reflections = 0;
  bool observation_in_flight = false;
  bool reflection_in_flight = false;
  std::uint32_t source_tokens = 0;
  std::uint32_t summary_tokens = 0;
  std::uint32_t saved_tokens = 0;
};

struct ContextLaneModel {
  bool visible = false;
  std::string owner_label;
  std::string toggle_hint;
  std::string model_label;
  std::string account_label;
  std::string quota_label;
  float context_ratio = 0.0f;
  std::string context_label;
  std::string usage_label;
  std::string cost_label;
  std::vector<std::string> bucket_labels;
  std::vector<std::string> memory_labels;
  RollingMemoryLaneModel rolling_memory;
};

ftxui::Component ContextLane(const std::shared_ptr<ContextLaneModel> &model);

} // namespace firmius::tui

#endif
