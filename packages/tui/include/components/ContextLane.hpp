#ifndef FIRMIUS_COMPONENTS_CONTEXT_LANE_HPP
#define FIRMIUS_COMPONENTS_CONTEXT_LANE_HPP

#include <cstdint>
#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

struct ContextBucket {
  std::string label;
  std::uint32_t tokens = 0;
  float ratio = 0.0f;
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
  std::vector<ContextBucket> context_buckets;
  std::vector<std::string> memory_labels;
};

ftxui::Component ContextLane(const std::shared_ptr<ContextLaneModel> &model);

} // namespace firmius::tui

#endif
