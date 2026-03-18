#ifndef FIRMIUS_COMPONENTS_PLAN_LANE_HPP
#define FIRMIUS_COMPONENTS_PLAN_LANE_HPP

#include "ActivePlanState.hpp"
#include <ftxui/component/component_base.hpp>
#include <memory>

namespace firmius::tui {

ftxui::Component PlanLane(const std::shared_ptr<PlanLaneModel> &model);

} // namespace firmius::tui

#endif
