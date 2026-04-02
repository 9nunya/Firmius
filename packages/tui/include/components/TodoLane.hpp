#ifndef FIRMIUS_COMPONENTS_TODO_LANE_HPP
#define FIRMIUS_COMPONENTS_TODO_LANE_HPP

#include "Enums.hpp"
#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {

struct TodoLaneRow {
  int id = 0;
  std::string text;
  firmius::shared::TodoStatus status = firmius::shared::TodoStatus::Pending;
};

struct TodoLaneModel {
  bool visible = false;
  std::string owner_label;
  bool show_chunk_header = false;
  std::string chunk_title;
  std::vector<TodoLaneRow> rows;
  std::string toggle_hint;
};

ftxui::Component TodoLane(const std::shared_ptr<TodoLaneModel> &model);

} // namespace firmius::tui

#endif
